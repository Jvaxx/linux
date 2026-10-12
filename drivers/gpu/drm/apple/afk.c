// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Sven Peter <sven@svenpeter.dev> */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kconfig.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/soc/apple/rtkit.h>

#include "afk.h"
#include "afkv2-codec.h"
#include "trace.h"

struct afk_receive_message_work {
	struct apple_dcp_afkep *ep;
	u64 message;
	struct work_struct work;
};

#define RBEP_TYPE GENMASK(63, 48)

enum rbep_msg_type {
	RBEP_INIT = 0x80,
	RBEP_INIT_ACK = 0xa0,
	RBEP_GETBUF = 0x89,
	RBEP_GETBUF_ACK = 0xa1,
	RBEP_INIT_TX = 0x8a,
	RBEP_INIT_RX = 0x8b,
	RBEP_START = 0xa3,
	RBEP_START_ACK = 0x86,
	RBEP_SEND = 0xa2,
	RBEP_RECV = 0x85,
	RBEP_SHUTDOWN = 0xc0,
	RBEP_SHUTDOWN_ACK = 0xc1,
};

#define BLOCK_SHIFT 6

#define GETBUF_SIZE GENMASK(31, 16)
#define GETBUF_TAG GENMASK(15, 0)
#define GETBUF_ACK_DVA GENMASK(47, 0)

#define INITRB_OFFSET GENMASK(47, 32)
#define INITRB_SIZE GENMASK(31, 16)
#define INITRB_TAG GENMASK(15, 0)

#define SEND_WPTR GENMASK(31, 0)

static void afk_send(struct apple_dcp_afkep *ep, u64 message)
{
	dcp_send_message(ep->dcp, ep->endpoint, message);
}

static void afk_release_context(void *data)
{
	struct apple_dcp_afkep *afkep = data;

	afk_quiesce(afkep);
	if (!afkep->dcp->retain_dma)
		kfree(afkep);
}

struct apple_dcp_afkep *afk_init(struct apple_dcp *dcp, u32 endpoint,
				 const struct apple_epic_service_ops *ops)
{
	struct apple_dcp_afkep *afkep;
	int ret, i;

	afkep = kzalloc_obj(*afkep);
	if (!afkep)
		return ERR_PTR(-ENOMEM);

	afkep->ops = ops;
	afkep->dcp = dcp;
	afkep->endpoint = endpoint;
	afkep->wq = alloc_ordered_workqueue("apple-dcp-afkep%02x",
					    WQ_MEM_RECLAIM, endpoint);
	if (!afkep->wq) {
		ret = -ENOMEM;
		goto out_free_afkep;
	}

	init_completion(&afkep->started);
	init_completion(&afkep->stopped);
	spin_lock_init(&afkep->lock);
	for (i = 0; i < AFK_MAX_CHANNEL; i++)
		spin_lock_init(&afkep->services[i].lock);
	ret = devm_add_action_or_reset(dcp->dev, afk_release_context, afkep);
	if (ret)
		return ERR_PTR(ret);

	return afkep;

out_free_afkep:
	kfree(afkep);
	return ERR_PTR(ret);
}

void afk_quiesce(struct apple_dcp_afkep *afkep)
{
	if (IS_ERR_OR_NULL(afkep))
		return;
	if (afkep->wq) {
		destroy_workqueue(afkep->wq);
		afkep->wq = NULL;
	}
	debugfs_remove_recursive(afkep->debugfs_entry);
	afkep->debugfs_entry = NULL;
}

void afk_shutdown(struct apple_dcp_afkep *afkep)
{
	unsigned long flags;

	afk_send(afkep, FIELD_PREP(RBEP_TYPE, RBEP_SHUTDOWN));
	int ret;

	ret = wait_for_completion_timeout(&afkep->stopped, msecs_to_jiffies(1000));
	if (ret <= 0) {
		dev_err(afkep->dcp->dev, "Timed out shutting down AFK endpoint %02x", afkep->endpoint);
	}

	spin_lock_irqsave(&afkep->lock, flags);
	afkep->stopping = true;
	spin_unlock_irqrestore(&afkep->lock, flags);
	afk_quiesce(afkep);
	if (afkep->rx_scratch) {
		devm_kfree(afkep->dcp->dev, afkep->rx_scratch);
		afkep->rx_scratch = NULL;
	}
}

int afk_start(struct apple_dcp_afkep *ep)
{
	int ret;

	reinit_completion(&ep->started);
	ret = apple_rtkit_start_ep(ep->dcp->rtk, ep->endpoint);
	if (ret)
		return ret;
	afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_INIT));

	ret = wait_for_completion_timeout(&ep->started, msecs_to_jiffies(1000));
	if (ret <= 0 && ep->dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL)
		dev_warn(ep->dcp->dev,
			 "external AFK startup ep=%#x tx_ready=%u rx_ready=%u bytes=%zu\n",
			 ep->endpoint, ep->txbfr.ready, ep->rxbfr.ready, ep->bfr_size);
	if (ret <= 0)
		return -ETIMEDOUT;
	else
		return 0;
}

static void afk_getbuf(struct apple_dcp_afkep *ep, u64 message)
{
	u32 size = FIELD_GET(GETBUF_SIZE, message) << BLOCK_SHIFT;
	u16 tag = FIELD_GET(GETBUF_TAG, message);
	u64 reply;

	trace_afk_getbuf(ep, size, tag);

	if (ep->bfr) {
		dev_err(ep->dcp->dev,
			"Got GETBUF message but buffer already exists\n");
		return;
	}

	/* An unstopped adopted session owns this ring until the machine resets. */
	if (READ_ONCE(ep->dcp->retain_dma))
		ep->bfr = dma_alloc_coherent(ep->dcp->dev, size, &ep->bfr_dma, GFP_KERNEL);
	else
		ep->bfr = dmam_alloc_coherent(ep->dcp->dev, size, &ep->bfr_dma, GFP_KERNEL);
	if (!ep->bfr) {
		dev_err(ep->dcp->dev, "Failed to allocate %d bytes buffer\n",
			size);
		return;
	}

	ep->bfr_size = size;
	ep->bfr_tag = tag;

	reply = FIELD_PREP(RBEP_TYPE, RBEP_GETBUF_ACK);
	reply |= FIELD_PREP(GETBUF_ACK_DVA, ep->bfr_dma);
	afk_send(ep, reply);
}

static void afk_init_rxtx(struct apple_dcp_afkep *ep, u64 message,
			  struct afk_ringbuffer *bfr)
{
	u32 base = FIELD_GET(INITRB_OFFSET, message) << BLOCK_SHIFT;
	u32 size = FIELD_GET(INITRB_SIZE, message) << BLOCK_SHIFT;
	u16 tag = FIELD_GET(INITRB_TAG, message);
	u32 bufsz, end, stride;
	u8 *hdr;

	if (tag != ep->bfr_tag) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: expected tag 0x%x but got 0x%x\n",
			ep->endpoint, ep->bfr_tag, tag);
		return;
	}

	if (bfr->ready) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: buffer is already initialized\n",
			ep->endpoint);
		return;
	}

	if (!ep->bfr || base >= ep->bfr_size || size < sizeof(u32)) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: invalid ring base 0x%x or size 0x%x\n",
			ep->endpoint, base, size);
		return;
	}

	end = base + size;
	if (end > ep->bfr_size) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: requested end 0x%x > max size 0x%lx\n",
			ep->endpoint, end, ep->bfr_size);
		return;
	}

	hdr = (u8 *)ep->bfr + base;
	dma_rmb();
	bufsz = le32_to_cpu(READ_ONCE(*(__le32 *)hdr));
	if (afk_ring_stride(size, bufsz, &stride)) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: invalid ring payload size 0x%x of 0x%x\n",
			ep->endpoint, bufsz, size);
		return;
	}
	if (bufsz < BIT(BLOCK_SHIFT)) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: ring cannot hold an entry\n",
			ep->endpoint);
		return;
	}
	if (bfr == &ep->rxbfr) {
		/* Receive dispatch cannot recurse on the ordered endpoint queue. */
		ep->rx_scratch = devm_kmalloc(ep->dcp->dev, bufsz, GFP_KERNEL);
		if (!ep->rx_scratch)
			return;
	}

	bfr->rptr = (__le32 *)(hdr + stride);
	bfr->wptr = (__le32 *)(hdr + 2 * stride);
	bfr->buf = hdr + 3 * stride;
	bfr->bufsz = bufsz;
	bfr->stride = stride;
	bfr->ready = true;

	if (ep->rxbfr.ready && ep->txbfr.ready)
		afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_START));
}

#if IS_ENABLED(CONFIG_DRM_APPLE_DEBUG)
static void afk_populate_service_debugfs(struct apple_epic_service *srv);
static void afk_remove_service_debugfs(struct apple_epic_service *srv);
#else
static void afk_populate_service_debugfs(struct apple_epic_service *srv)
{
}
static void afk_remove_service_debugfs(struct apple_epic_service *srv)
{
}
#endif

static const struct apple_epic_service_ops *
afk_match_service(struct apple_dcp_afkep *ep, const char *name)
{
	const struct apple_epic_service_ops *ops;

	if (!name || !name[0])
		return NULL;
	if (!ep->ops)
		return NULL;

	for (ops = ep->ops; ops->name[0]; ops++) {
		if (strcmp(ops->name, name))
			continue;

		return ops;
	}

	return NULL;
}

static struct apple_epic_service *afk_epic_find_service(struct apple_dcp_afkep *ep,
						     u32 channel)
{
	struct apple_epic_service *service;
	u32 i;

	for (i = 0; i < ep->num_channels; i++) {
		service = &ep->services[i];
		if (afk_service_matches(service, channel))
			return service;
	}

	return NULL;
}

static int afk_send_epic_token(struct apple_dcp_afkep *ep, u32 channel, u16 tag,
			       enum epic_type etype, enum epic_category ecat, u8 stype,
			       const void *payload, size_t payload_len, u64 token);

static void afk_recv_handle_init(struct apple_dcp_afkep *ep, u32 channel,
				 u8 *payload, size_t payload_size)
{
	char name[32];
	s64 epic_unit = -1;
	struct apple_epic_service *service = NULL;
	u32 ch_idx;
	const char *service_name = name;
	const char *epic_name = NULL, *epic_class = NULL;
	const struct apple_epic_service_ops *ops;
	struct dcp_parse_ctx ctx;
	u8 *props = payload + sizeof(name);
	size_t props_size = payload_size - sizeof(name);

	if (afk_epic_find_service(ep, channel))
		return;

	if (payload_size < sizeof(name)) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: payload too small: %lx\n",
			ep->endpoint, payload_size);
		return;
	}

	strscpy(name, payload, sizeof(name));

	/*
	 * in DCP firmware 13.2 DCP reports interface-name as name which starts
	 * with "dispext%d" using -1 s ID for "dcp". In the 12.3 firmware
	 * EPICProviderClass was used. If the init call has props parse them and
	 * use EPICProviderClass to match the service.
	 */
	if (props_size > 36) {
		int ret = parse(props, props_size, &ctx);
		if (ret) {
			dev_err(ep->dcp->dev,
				"AFK[ep:%02x]: Failed to parse service init props for %s\n",
				ep->endpoint, name);
			return;
		}
		ret = parse_epic_service_init(&ctx, &epic_name, &epic_class, &epic_unit,
					      ep->dcp->fw_compat == DCP_FIRMWARE_H17P);
		if (ret) {
			dev_err(ep->dcp->dev,
				"AFK[ep:%02x]: failed to extract init props: %d\n",
				ep->endpoint, ret);
			return;
		}
		service_name = epic_class;
	} else {
            service_name = name;
        }

	if (ep->match_epic_name)
		ops = afk_match_service(ep, epic_name);
	else
		ops = afk_match_service(ep, service_name);

	if (!ops) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: unable to match service %s on channel %d\n",
			ep->endpoint, service_name, channel);
		goto free;
	}

	for (ch_idx = 0; ch_idx < AFK_MAX_CHANNEL; ch_idx++) {
		struct apple_epic_service *candidate = &ep->services[ch_idx];

		if (afk_service_reinit(candidate, ep, ops, channel)) {
			service = candidate;
			break;
		}
	}
	if (!service) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: no reusable service slots\n",
			ep->endpoint);
		goto free;
	}
	ep->num_channels = max(ep->num_channels, ch_idx + 1);
	if (ep->wire_v2) {
		int ret = afk_send_epic(ep, channel, 0, EPIC_TYPE_NOTIFY,
					EPIC_CAT_REPORT, AFKV2_OPEN, NULL, 0);

		if (ret) {
			unsigned long flags;

			spin_lock_irqsave(&service->lock, flags);
			service->enabled = false;
			service->torndown = true;
			spin_unlock_irqrestore(&service->lock, flags);
			dev_err(ep->dcp->dev, "AFKV2 interface %u OPEN failed: %d\n",
				channel, ret);
			goto free;
		}
	}
	ops->init(service, epic_name, epic_class, epic_unit);
	dev_info(ep->dcp->dev, "AFK[ep:%02x]: new service %s on channel %d\n",
		 ep->endpoint, service_name, channel);

	afk_populate_service_debugfs(service);

free:
	kfree(epic_name);
	kfree(epic_class);
}

static void afk_recv_handle_teardown(struct apple_dcp_afkep *ep, u32 channel)
{
	struct apple_epic_service *service;
	const struct apple_epic_service_ops *ops;
	unsigned long flags;

	service = afk_epic_find_service(ep, channel);
	if (!service) {
		dev_warn(ep->dcp->dev, "AFK[ep:%02x]: teardown for disabled channel %u\n",
			 ep->endpoint, channel);
		return;
	}

	afk_remove_service_debugfs(service);

	spin_lock_irqsave(&service->lock, flags);
	if (service->torndown) {
		spin_unlock_irqrestore(&service->lock, flags);
		return;
	}
	/* Outstanding commands remain discoverable until their replies arrive. */
	service->torndown = true;
	service->enabled = false;
	ops = service->ops;
	spin_unlock_irqrestore(&service->lock, flags);

	if (ops->teardown)
		ops->teardown(service);
}

static void afk_recv_handle_reply(struct apple_dcp_afkep *ep, u32 channel,
				  u16 tag, void *payload, size_t payload_size)
{
	struct epic_cmd *cmd = payload;
	struct apple_epic_service *service;
	unsigned long flags;
	u8 idx = tag & (ep->wire_v2 ? 0xf : 0xff);
	void *rxbuf, *txbuf;
	dma_addr_t rxbuf_dma, txbuf_dma;
	size_t rxlen, txlen;

	service = afk_epic_find_service(ep, channel);
	if (!service) {
		dev_warn(ep->dcp->dev, "AFK[ep:%02x]: command reply on disabled channel %u\n",
			 ep->endpoint, channel);
		return;
	}

	if (payload_size < sizeof(*cmd)) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: command reply on channel %d too small: %ld\n",
			ep->endpoint, channel, payload_size);
		return;
	}

	if (idx >= MAX_PENDING_CMDS) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: command reply on channel %d out of range: %d\n",
			ep->endpoint, channel, idx);
		return;
	}

	spin_lock_irqsave(&service->lock, flags);
	if (!test_bit(idx, service->cmd_map) || service->cmds[idx].done) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: command reply on channel %d already handled\n",
			ep->endpoint, channel);
		spin_unlock_irqrestore(&service->lock, flags);
		return;
	}

	if (tag != service->cmds[idx].tag) {
		dev_err(ep->dcp->dev,
			"AFK[ep:%02x]: command reply on channel %d has invalid tag: expected 0x%04x != 0x%04x\n",
			ep->endpoint, channel, tag, service->cmds[idx].tag);
		spin_unlock_irqrestore(&service->lock, flags);
		return;
	}

	service->cmds[idx].done = true;
	service->cmds[idx].retcode = le32_to_cpu(cmd->retcode);
	if (service->cmds[idx].free_on_ack) {
		/* defer freeing until we're no longer in atomic context */
		rxbuf = service->cmds[idx].rxbuf;
		txbuf = service->cmds[idx].txbuf;
		rxlen = service->cmds[idx].rxlen;
		txlen = service->cmds[idx].txlen;
		rxbuf_dma = service->cmds[idx].rxbuf_dma;
		txbuf_dma = service->cmds[idx].txbuf_dma;
		bitmap_release_region(service->cmd_map, idx, 0);
	} else {
		rxbuf = txbuf = NULL;
		rxlen = txlen = 0;
	}
	if (service->cmds[idx].completion)
		complete(service->cmds[idx].completion);

	spin_unlock_irqrestore(&service->lock, flags);

	if (rxbuf && rxlen)
		dma_free_coherent(ep->dcp->dev, rxlen, rxbuf, rxbuf_dma);
	if (txbuf && txlen)
		dma_free_coherent(ep->dcp->dev, txlen, txbuf, txbuf_dma);
}

struct epic_std_service_ap_call {
	__le32 unk0;
	__le32 unk1;
	__le32 type;
	__le32 len;
	__le32 magic;
	u8 _unk[48];
} __attribute__((packed));

static void afk_recv_handle_std_service(struct apple_dcp_afkep *ep, u32 channel,
					u32 type, struct epic_hdr *ehdr,
					struct epic_sub_hdr *eshdr,
					void *payload, size_t payload_size)
{
	struct apple_epic_service *service = afk_epic_find_service(ep, channel);

	if (!service) {
		dev_warn(ep->dcp->dev,
			 "AFK[ep:%02x]: std service notify on disabled channel %u\n",
			 ep->endpoint, channel);
		return;
	}
	if (service->torndown) {
		dev_warn(ep->dcp->dev,
			 "AFK[ep:%02x]: std service notify on torn down service "
			 "(chan:%u)\n", ep->endpoint, channel);
		return;
	}

	if (type == EPIC_TYPE_NOTIFY && eshdr->category == EPIC_CAT_NOTIFY) {
		struct epic_std_service_ap_call *call = payload;
		size_t call_size;
		void *reply;
		int ret;

		if (payload_size < sizeof(*call))
			return;

		call_size = le32_to_cpu(call->len);
		if (payload_size < sizeof(*call) + call_size)
			return;

		if (!service->ops->call)
			return;
		reply = kzalloc(payload_size, GFP_KERNEL);
		if (!reply)
			return;

		ret = service->ops->call(service, le32_to_cpu(call->type),
					 payload + sizeof(*call), call_size,
					 reply + sizeof(*call), call_size);
		if (ret) {
			dev_warn(ep->dcp->dev,
				 "AFK[ep:%02x]: %s (chan:%u) apcall %u handler returned %d, sending NO reply\n",
				 ep->endpoint, service->ops->name, channel,
				 le32_to_cpu(call->type), ret);
			kfree(reply);
			return;
		}

		memcpy(reply, call, sizeof(*call));
		afk_send_epic(ep, channel, le16_to_cpu(eshdr->tag),
			      EPIC_TYPE_NOTIFY_ACK, EPIC_CAT_REPLY,
			      EPIC_SUBTYPE_STD_SERVICE, reply, payload_size);
		kfree(reply);

		return;
	}

	if (type == EPIC_TYPE_NOTIFY && eshdr->category == EPIC_CAT_REPORT) {
		if (service->ops->report)
			service->ops->report(service, le16_to_cpu(eshdr->type),
					     payload, payload_size);
		return;
	}

	dev_err(ep->dcp->dev,
		"AFK[ep:%02x]: channel %d received unhandled standard service message: %x / %x\n",
		ep->endpoint, channel, type, eshdr->category);
	if (ep->dcp->hw.iomfb_method_profile !=
	    DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL)
		print_hex_dump(KERN_INFO, "AFK: ", DUMP_PREFIX_NONE, 16, 1,
			       payload, payload_size, true);
}

static bool afk_validate_h17p_header(const u8 *data, size_t size)
{
	/* The measured length counts bytes after the first eight header bytes. */
	return size >= sizeof(struct epic_hdr) +
		       sizeof(struct epic_sub_hdr_h17p) &&
	       get_unaligned_le32(data + 4) == size - 8;
}

static void afk_v2_status(struct apple_dcp_afkep *ep,
			  const struct afkv2_frame *frame,
			  const struct afkv2_command *command, u32 status)
{
	u8 record[AFKV2_HEADER_SIZE + AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE];
	size_t size;
	int ret;

	ret = afkv2_encode_error_reply(record, sizeof(record), 0, frame, command,
				       status, &size);
	if (!ret)
		ret = afk_send_epic_token(ep, frame->interface, command->id,
					  EPIC_TYPE_REPLY, EPIC_CAT_REPLY, frame->type,
					  record + AFKV2_HEADER_SIZE,
					  size - AFKV2_HEADER_SIZE, frame->token);
	if (ret)
		dev_warn(ep->dcp->dev, "AFKV2 error reply failed: %d\n", ret);
}

static int afk_v2_access_owned(struct apple_epic_service *service, u64 dva,
			       void *buffer, size_t size, bool to_peer, bool copy)
{
	unsigned long flags;
	unsigned int i;
	int ret = -EACCES;

	if (!service || !service->ep->wire_v2 || size > SZ_1M || (copy && size && !buffer))
		return -EINVAL;
	if (!size)
		return 0;
	spin_lock_irqsave(&service->lock, flags);
	for (i = 0; i < MAX_PENDING_CMDS; i++) {
		struct epic_cmd_info *cmd = &service->cmds[i];
		void *owned = NULL;
		u64 offset;

		if (!test_bit(i, service->cmd_map) || cmd->done)
			continue;
		if (cmd->rxbuf && afkv2_owned_span(cmd->rxbuf_dma, cmd->rxlen, dva, size)) {
			offset = dva - cmd->rxbuf_dma;
			owned = cmd->rxbuf + offset;
		} else if (cmd->txbuf && afkv2_owned_span(cmd->txbuf_dma, cmd->txlen, dva, size)) {
			offset = dva - cmd->txbuf_dma;
			owned = cmd->txbuf + offset;
		}
		if (!owned)
			continue;
		if (!copy) {
			ret = 0;
			break;
		}
		if (to_peer) {
			memcpy(owned, buffer, size);
			dma_wmb();
		} else {
			dma_rmb();
			memcpy(buffer, owned, size);
		}
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&service->lock, flags);
	if (ret)
		dev_warn_ratelimited(service->ep->dcp->dev,
				     "AFKV2 unowned buffer: ep=%u interface=%u size=%zu write=%u\n",
				     service->ep->endpoint, service->channel, size, to_peer);
	return ret;
}

int afk_v2_copy_owned(struct apple_epic_service *service, u64 dva,
		      void *buffer, size_t size, bool to_peer)
{
	return afk_v2_access_owned(service, dva, buffer, size, to_peer, true);
}

static void afk_v2_callback(struct apple_dcp_afkep *ep,
			    const struct afkv2_frame *frame,
			    const struct afkv2_command *command)
{
	struct apple_epic_service *service;
	const struct epic_service_call *call;
	const u8 *request = command->inline_data;
	u8 *owned_request = NULL, *reply = NULL;
	size_t request_size = command->oob ? command->tx_size : command->inline_size;
	size_t body_size, reply_size;
	u32 status = 0xe00002c7;
	int ret;

	if (frame->type != EPIC_SUBTYPE_STD_SERVICE ||
	    request_size < sizeof(*call) + sizeof(u32) ||
	    request_size > SZ_1M - AFKV2_COMMAND_SIZE ||
	    request_size > command->value ||
	    (command->oob && request_size > command->rx_size))
		goto reject;
	service = afk_service_get(afk_epic_find_service(ep, frame->interface));
	if (!service || READ_ONCE(service->torndown) ||
	    (!service->ops->call && !service->ops->call_v2)) {
		if (service)
			afk_service_put(service);
		goto reject;
	}
	if (command->oob) {
		/* Reject an unowned response destination before any callback side effect. */
		ret = afk_v2_access_owned(service, command->rx_address, NULL,
					  request_size, true, false);
		if (ret)
			goto release;
		owned_request = kmalloc(request_size, GFP_KERNEL);
		if (!owned_request) {
			status = 0xe00002bd;
			goto release;
		}
		ret = afk_v2_copy_owned(service, command->tx_address,
					owned_request, request_size, false);
		if (ret)
			goto release;
		request = owned_request;
	}
	call = (const void *)request;
	body_size = request_size - sizeof(*call);
	if ((le16_to_cpu(call->group) && !service->ops->call_v2) ||
	    le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC ||
	    le32_to_cpu(call->data_len) != body_size)
		goto release;
	reply = kzalloc(AFKV2_COMMAND_SIZE + request_size, GFP_KERNEL);
	if (!reply) {
		status = 0xe00002bd;
		goto release;
	}
	reply[1] = command->id;
	memcpy(reply + AFKV2_COMMAND_SIZE, call, sizeof(*call));
	if (service->ops->call_v2)
		ret = service->ops->call_v2(service, le16_to_cpu(call->group),
					  le32_to_cpu(call->command),
					  request + sizeof(*call), body_size,
					  reply + AFKV2_COMMAND_SIZE + sizeof(*call), body_size);
	else
		ret = service->ops->call(service, le32_to_cpu(call->command),
					 request + sizeof(*call), body_size,
					 reply + AFKV2_COMMAND_SIZE + sizeof(*call), body_size);
	if (ret)
		goto release;
	reply_size = AFKV2_COMMAND_SIZE + request_size;
	if (command->oob) {
		ret = afk_v2_copy_owned(service, command->rx_address,
					reply + AFKV2_COMMAND_SIZE, request_size, true);
		if (ret)
			goto release;
		ret = afkv2_encode_oob_command(reply, reply_size, command->id,
					       command->rx_address, request_size,
					       command->tx_address, command->tx_size);
		if (ret)
			goto release;
		afkv2_put32(reply + 4, 0);
		reply_size = AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE;
	}
	ret = afk_send_epic_token(ep, frame->interface, command->id, EPIC_TYPE_REPLY,
				  EPIC_CAT_REPLY, frame->type, reply, reply_size, frame->token);
	kfree(reply);
	kfree(owned_request);
	afk_service_put(service);
	if (ret)
		dev_warn(ep->dcp->dev, "AFKV2 callback reply failed: %d\n", ret);
	return;
release:
	kfree(reply);
	kfree(owned_request);
	afk_service_put(service);
reject:
	afk_v2_status(ep, frame, command, status);
}

static void afk_v2_receive(struct apple_dcp_afkep *ep, u32 channel, u32 type,
			   const void *data, size_t size)
{
	struct afkv2_frame frame;
	struct afkv2_command response;
	struct apple_epic_service *service;
	struct epic_cmd legacy = {};
	unsigned long flags;
	unsigned int slot;
	int ret;

	if (channel || type || afkv2_decode_frame(data, size, false, &frame) ||
	    !frame.interface)
		return;
	if (frame.category == AFKV2_REPORT) {
		if (frame.type == EPIC_SUBTYPE_ANNOUNCE_H17P)
			afk_recv_handle_init(ep, frame.interface, (void *)frame.payload,
					     frame.payload_size);
		else if (frame.type == AFKV2_CLOSE && frame.interface_flags == 1)
			afk_recv_handle_teardown(ep, frame.interface);
		return;
	}
	if (afkv2_decode_command(frame.payload, frame.payload_size, &response))
		return;
	if (frame.category == AFKV2_COMMAND) {
		afk_v2_callback(ep, &frame, &response);
		return;
	}
	service = afk_epic_find_service(ep, frame.interface);
	if (!service)
		return;
	spin_lock_irqsave(&service->lock, flags);
	if (bitmap_empty(service->cmd_map, MAX_PENDING_CMDS)) {
		spin_unlock_irqrestore(&service->lock, flags);
		return;
	}
	service->users++;
	spin_unlock_irqrestore(&service->lock, flags);
	slot = response.id & 0xf;
	spin_lock_irqsave(&service->lock, flags);
	ret = -EPROTO;
	if (slot < MAX_PENDING_CMDS && test_bit(slot, service->cmd_map) &&
	    !service->cmds[slot].done && response.id == service->cmds[slot].tag)
		ret = afkv2_check_response(&frame, &response, service->channel,
					   service->cmds[slot].type, service->cmds[slot].tag,
					   service->cmds[slot].rxbuf_dma,
					   service->cmds[slot].rxlen,
					   service->cmds[slot].txbuf_dma,
					   service->cmds[slot].txlen);
	if (!ret)
		service->cmds[slot].reply_length = response.value ? 0 : response.rx_size;
	spin_unlock_irqrestore(&service->lock, flags);
	if (!ret) {
		legacy.retcode = cpu_to_le32(response.value);
		afk_recv_handle_reply(ep, frame.interface, response.id,
				      &legacy, sizeof(legacy));
	} else {
		dev_warn(ep->dcp->dev, "AFKV2 reply rejected: %d\n", ret);
	}
	afk_service_put(service);
}

static void afk_recv_handle(struct apple_dcp_afkep *ep, u32 channel, u32 type,
			    u8 *data, size_t data_size)
{
	struct apple_epic_service *service;
	struct epic_hdr *ehdr = (struct epic_hdr *)data;
	struct epic_sub_hdr *eshdr =
		(struct epic_sub_hdr *)(data + sizeof(*ehdr));
	struct epic_sub_hdr h17p_eshdr;
	size_t hdr_len = sizeof(*ehdr) + sizeof(*eshdr);
	u16 subtype;
	u8 *payload;
	size_t payload_size;

	if (ep->wire_v2) {
		afk_v2_receive(ep, channel, type, data, data_size);
		return;
	}

	if (ep->dcp->fw_compat == DCP_FIRMWARE_H17P) {
		const struct epic_sub_hdr_h17p *c =
			(const struct epic_sub_hdr_h17p *)(data + sizeof(*ehdr));

		hdr_len = sizeof(*ehdr) + sizeof(*c);
		if (data_size < hdr_len)
			goto too_small;
		if (ep->dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
		    !afk_validate_h17p_header(data, data_size)) {
			dev_err(ep->dcp->dev,
				"AFK[ep:%02x]: invalid compact message length\n",
				ep->endpoint);
			return;
		}

		/*
		 * H17P multiplexes every service of an endpoint onto queue
		 * channel 0 and carries the real channel in the EPIC header
		 * instead: byte 0 is a sequence counter, byte 1 a flags byte
		 * and bytes 2-3 the channel.  ep:20 announces "system" as
		 * channel 1 and "powerlog-service" as channel 3, then sends the
		 * mNits/uAmps/iDAC backlight reports on channel 3.  Taking the
		 * queue channel at face value binds "system" to 0 and makes
		 * every later announce and report look like a duplicate.
		 */
		channel = le16_to_cpup((__le16 *)(data + 2));

		/*
		 * Normalise into the wide sub-header the rest of this file
		 * expects, mapping H17P's announce subtype onto the canonical
		 * one so no downstream check has to know the difference.
		 */
		memset(&h17p_eshdr, 0, sizeof(h17p_eshdr));
		h17p_eshdr.category = c->category;
		h17p_eshdr.type = cpu_to_le16(c->type == EPIC_SUBTYPE_ANNOUNCE_H17P ?
					      EPIC_SUBTYPE_ANNOUNCE : c->type);
		h17p_eshdr.tag = cpu_to_le16(le32_to_cpu(c->tag));
		eshdr = &h17p_eshdr;
	}

	if (data_size < hdr_len)
		goto too_small;

	subtype = le16_to_cpu(eshdr->type);
	payload = data + hdr_len;
	payload_size = data_size - hdr_len;

	trace_afk_recv_handle(ep, channel, type, data_size, ehdr, eshdr);
	if (ep->dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL &&
	    ep->experimental_rx_logs < 32) {
		ep->experimental_rx_logs++;
		dev_info(ep->dcp->dev,
			 "external AFK ep=%u channel=%u type=%u subtype=%u category=%u bytes=%zu\n",
			 ep->endpoint, channel, type, subtype, eshdr->category,
			 data_size);
	}


	service = afk_epic_find_service(ep, channel);

	if (!service) {
		if (type != EPIC_TYPE_NOTIFY && type != EPIC_TYPE_REPLY) {
			dev_err(ep->dcp->dev,
				"AFK[ep:%02x]: expected notify but got 0x%x on channel %d\n",
				ep->endpoint, type, channel);
			return;
		}
		if (eshdr->category != EPIC_CAT_REPORT) {
			dev_err(ep->dcp->dev,
				"AFK[ep:%02x]: expected report but got 0x%x on channel %d\n",
				ep->endpoint, eshdr->category, channel);
			return;
		}
		if (subtype == EPIC_SUBTYPE_TEARDOWN) {
			dev_dbg(ep->dcp->dev,
				"AFK[ep:%02x]: teardown without service on channel %d\n",
				ep->endpoint, channel);
			return;
		}
		if (subtype != EPIC_SUBTYPE_ANNOUNCE) {
			dev_err(ep->dcp->dev,
				"AFK[ep:%02x]: expected announce but got 0x%x on channel %d\n",
				ep->endpoint, subtype, channel);
			return;
		}

		return afk_recv_handle_init(ep, channel, payload, payload_size);
	}

	if (!service) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: channel %d has no service\n",
			ep->endpoint, channel);
		return;
	}

	if (type == EPIC_TYPE_NOTIFY && eshdr->category == EPIC_CAT_REPORT &&
	    subtype == EPIC_SUBTYPE_TEARDOWN)
		return afk_recv_handle_teardown(ep, channel);

	if (type == EPIC_TYPE_REPLY && eshdr->category == EPIC_CAT_REPLY)
		return afk_recv_handle_reply(ep, channel,
					     le16_to_cpu(eshdr->tag), payload,
					     payload_size);

	if (subtype == EPIC_SUBTYPE_STD_SERVICE)
		return afk_recv_handle_std_service(
			ep, channel, type, ehdr, eshdr, payload, payload_size);

	dev_err(ep->dcp->dev, "AFK[ep:%02x]: channel %d received unhandled message "
		"(type %x subtype %x)\n", ep->endpoint, channel, type, subtype);
	if (ep->dcp->hw.iomfb_method_profile !=
	    DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL)
		print_hex_dump(KERN_INFO, "AFK: ", DUMP_PREFIX_NONE, 16, 1,
			       payload, payload_size, true);
	return;

too_small:
	dev_err(ep->dcp->dev, "AFK[ep:%02x]: payload too small: %lx\n",
		ep->endpoint, data_size);
}

static bool afk_recv(struct apple_dcp_afkep *ep)
{
	struct afk_qe *hdr;
	u32 rptr, wptr;
	u32 magic, size, channel, type;

	if (!ep->rxbfr.ready) {
		dev_err(ep->dcp->dev, "AFK[ep:%02x]: got RECV but not ready\n",
			ep->endpoint);
		return false;
	}

	rptr = le32_to_cpu(*ep->rxbfr.rptr);
	wptr = le32_to_cpu(*ep->rxbfr.wptr);
	trace_afk_recv_rwptr_pre(ep, rptr, wptr);

	if (rptr == wptr)
		return false;

	if (rptr > (ep->rxbfr.bufsz - sizeof(*hdr))) {
		dev_warn(ep->dcp->dev,
			 "AFK[ep:%02x]: rptr out of bounds: 0x%x > 0x%lx\n",
			 ep->endpoint, rptr, ep->rxbfr.bufsz - sizeof(*hdr));
		return false;
	}

	dma_rmb();

	hdr = ep->rxbfr.buf + rptr;
	magic = le32_to_cpu(hdr->magic);
	size = le32_to_cpu(hdr->size);
	trace_afk_recv_qe(ep, rptr, magic, size);

	if (magic != QE_MAGIC) {
		dev_warn(ep->dcp->dev, "AFK[ep:%02x]: invalid queue entry magic: 0x%x\n",
			 ep->endpoint, magic);
		return false;
	}

	/*
	 * If there's not enough space for the payload the co-processor inserted
	 * the current dummy queue entry and we have to advance to the next one
	 * which will contain the real data.
	*/
	if (rptr + size + sizeof(*hdr) > ep->rxbfr.bufsz) {
		rptr = 0;
		hdr = ep->rxbfr.buf + rptr;
		magic = le32_to_cpu(hdr->magic);
		size = le32_to_cpu(hdr->size);
		trace_afk_recv_qe(ep, rptr, magic, size);

		if (magic != QE_MAGIC) {
			dev_warn(ep->dcp->dev,
				 "AFK[ep:%02x]: invalid next queue entry magic: 0x%x\n",
				 ep->endpoint, magic);
			return false;
		}
	}

	if (rptr + size + sizeof(*hdr) > ep->rxbfr.bufsz) {
		dev_warn(ep->dcp->dev,
			 "AFK[ep:%02x]: queue entry out of bounds: 0x%lx > 0x%lx\n",
			 ep->endpoint, rptr + size + sizeof(*hdr), ep->rxbfr.bufsz);
		return false;
	}

	channel = le32_to_cpu(hdr->channel);
	type = le32_to_cpu(hdr->type);
	/*
	 * Publishing rptr releases this entry to the firmware. Handlers can
	 * sleep and send replies, so retain a private copy before allowing
	 * the producer to reuse the ring storage.
	 */
	memcpy(ep->rx_scratch, hdr->data, size);

	rptr = afk_ring_advance(rptr, sizeof(*hdr) + size,
				ep->rxbfr.bufsz, ep->rxbfr.stride);

	dma_mb();

	*ep->rxbfr.rptr = cpu_to_le32(rptr);
	trace_afk_recv_rwptr_post(ep, rptr, wptr);

	afk_recv_handle(ep, channel, type, ep->rx_scratch, size);

	return true;
}

static void afk_receive_message_worker(struct work_struct *work_)
{
	struct afk_receive_message_work *work;
	u16 type;

	work = container_of(work_, struct afk_receive_message_work, work);

	type = FIELD_GET(RBEP_TYPE, work->message);
	if (work->ep->dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL &&
	    (work->ep->endpoint == DPTX_ENDPOINT ||
	     work->ep->endpoint == DPTX_ENDPOINT_V2) && type != RBEP_RECV)
		dev_info_ratelimited(work->ep->dcp->dev,
				     "external AFK control ep=%#x type=%#x\n",
				     work->ep->endpoint, type);
	switch (type) {
	case RBEP_INIT_ACK:
		break;

	case RBEP_START_ACK:
		complete_all(&work->ep->started);
		break;

	case RBEP_SHUTDOWN_ACK:
		complete_all(&work->ep->stopped);
		break;

	case RBEP_GETBUF:
		afk_getbuf(work->ep, work->message);
		break;

	case RBEP_INIT_TX:
		afk_init_rxtx(work->ep, work->message, &work->ep->txbfr);
		break;

	case RBEP_INIT_RX:
		afk_init_rxtx(work->ep, work->message, &work->ep->rxbfr);
		break;

	case RBEP_RECV:
		while (afk_recv(work->ep))
			;
		break;

	default:
		dev_err(work->ep->dcp->dev,
			"Received unknown AFK message type: 0x%x\n", type);
	}

	kfree(work);
}

int afk_receive_message(struct apple_dcp_afkep *ep, u64 message)
{
	struct afk_receive_message_work *work;
	unsigned long flags;

	if (!ep)
		return -ENODEV;

	// TODO: comment why decoupling from rtkit thread is required here
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	work->ep = ep;
	work->message = message;
	INIT_WORK(&work->work, afk_receive_message_worker);
	spin_lock_irqsave(&ep->lock, flags);
	if (ep->stopping) {
		spin_unlock_irqrestore(&ep->lock, flags);
		kfree(work);
		return -ESHUTDOWN;
	}
	queue_work(ep->wq, &work->work);
	spin_unlock_irqrestore(&ep->lock, flags);

	return 0;
}

static int afk_send_epic_token(struct apple_dcp_afkep *ep, u32 channel, u16 tag,
			       enum epic_type etype, enum epic_category ecat, u8 stype,
			       const void *payload, size_t payload_len, u64 token)
{
	u32 rptr, wptr;
	struct afk_qe *hdr, *hdr2;
	struct epic_hdr *ehdr;
	struct epic_sub_hdr *eshdr;
	unsigned long flags;
	size_t total_epic_size, total_size;
	u8 v2_command[AFKV2_COMMAND_SIZE + AFKV2_OOB_SIZE];
	u8 v2_category = 0;
	u16 v2_flags = 0;
	bool compact = !ep->wire_v2 && ep->dcp->hw.iomfb_method_profile ==
		DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL;
	int ret;

	/*
	 * H17P frames EPIC messages differently (see afk_recv_handle()): an
	 * 8-byte sub-header, with the service channel carried in the EPIC
	 * header on queue channel 0.  Only the receive side of that format is
	 * known, and the 24-byte framing below would be misparsed, so send
	 * nothing for the established internal profile. The external-only
	 * experiment below tests receive-layout symmetry. Internal callers are: the
	 * callers are optional (verbose firmware logging, EDID copy, audio,
	 * DPTX, debugfs) or replies to calls only DPTX services make.
	 */
	if (ep->dcp->fw_compat == DCP_FIRMWARE_H17P && !compact && !ep->wire_v2) {
		dev_dbg_once(ep->dcp->dev,
			     "AFK[ep:%02x]: not sending EPIC message on H17P\n",
			     ep->endpoint);
		return -EOPNOTSUPP;
	}

	/* Only existing DPTX/EDID service commands participate in this trial. */
	if (compact &&
	    (channel > U16_MAX || payload_len > SZ_1M ||
	     (payload_len && !payload) || stype != EPIC_SUBTYPE_STD_SERVICE ||
	     (ep->endpoint != DPTX_ENDPOINT && ep->endpoint != DPAVSERV_ENDPOINT) ||
	     !((ecat == EPIC_CAT_COMMAND && etype == EPIC_TYPE_COMMAND &&
		payload_len == sizeof(struct epic_cmd)) ||
	       (ecat == EPIC_CAT_REPLY &&
		(etype == EPIC_TYPE_REPLY || etype == EPIC_TYPE_NOTIFY_ACK)))))
		return -EOPNOTSUPP;

	if (ep->wire_v2) {
		if (!channel || channel > U16_MAX || tag > U8_MAX || payload_len > SZ_1M)
			return -EINVAL;
		if (ecat == EPIC_CAT_REPORT) {
			if ((stype != AFKV2_OPEN && stype != AFKV2_CLOSE) || payload_len)
				return -EOPNOTSUPP;
			v2_flags = 1;
		} else if (ecat == EPIC_CAT_COMMAND) {
			const struct epic_cmd *cmd = payload;

			if (stype != EPIC_SUBTYPE_STD_SERVICE || !cmd ||
			    payload_len != sizeof(*cmd))
				return -EINVAL;
			ret = afkv2_encode_oob_command(v2_command, sizeof(v2_command), tag,
						       le64_to_cpu(cmd->rxbuf),
						       le32_to_cpu(cmd->rxlen),
						       le64_to_cpu(cmd->txbuf),
						       le32_to_cpu(cmd->txlen));
			if (ret)
				return ret;
			payload = v2_command;
			payload_len = sizeof(v2_command);
			v2_category = AFKV2_COMMAND;
		} else if (ecat == EPIC_CAT_REPLY) {
			v2_category = AFKV2_RESPONSE;
		} else {
			return -EOPNOTSUPP;
		}
	}

	spin_lock_irqsave(&ep->lock, flags);
	if (ep->stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}

	if ((compact || ep->wire_v2) &&
	    (!ep->txbfr.ready || !ep->txbfr.rptr || !ep->txbfr.wptr || !ep->txbfr.buf)) {
		ret = -ESHUTDOWN;
		goto out;
	}

	dma_rmb();
	rptr = le32_to_cpu(*ep->txbfr.rptr);
	wptr = le32_to_cpu(*ep->txbfr.wptr);
	trace_afk_send_rwptr_pre(ep, rptr, wptr);
	total_epic_size = sizeof(*ehdr) +
		((compact || ep->wire_v2) ? sizeof(struct epic_sub_hdr_h17p) : sizeof(*eshdr)) +
		payload_len;
	total_size = sizeof(*hdr) + total_epic_size;

	if (compact || ep->wire_v2) {
		u32 available, needed;

		if (!ep->txbfr.ready || !ep->txbfr.stride ||
		    (ep->txbfr.stride & (ep->txbfr.stride - 1)) ||
		    rptr >= ep->txbfr.bufsz || wptr >= ep->txbfr.bufsz ||
		    !IS_ALIGNED(rptr, ep->txbfr.stride) ||
		    !IS_ALIGNED(wptr, ep->txbfr.stride) ||
		    total_size >= ep->txbfr.bufsz) {
			ret = -EPROTO;
			goto out;
		}
		available = rptr > wptr ? rptr - wptr : ep->txbfr.bufsz - wptr + rptr;
		needed = ALIGN(total_size, ep->txbfr.stride);
		if (total_size > ep->txbfr.bufsz - wptr)
			needed += ep->txbfr.bufsz - wptr;
		/* Retain an empty slot; equal read/write pointers mean empty. */
		if (needed >= available) {
			ret = -ENOSPC;
			goto out;
		}
	}

	hdr = hdr2 = NULL;

	/*
	 * We need to figure out how to place the entire headers and payload
	 * into the ring buffer:
	 * - If the write pointer is in front of the read pointer we just need
	 *   enough space inbetween to store everything.
	 * - If the read pointer has already wrapper around the end of the
	 *   buffer we can
	 *    a) either store the entire payload at the writer pointer if
	 *       there's enough space until the end,
	 *    b) or just store the queue entry at the write pointer to indicate
	 *       that we need to wrap to the start and then store the headers
	 *       and the payload at the beginning of the buffer. The queue
	 *       header has to be store twice in this case.
	 * In either case we have to ensure that there's always enough space
	 * so that we don't accidentally overwrite other buffers.
	 */
	if (wptr < rptr) {
		/*
		 * If wptr < rptr we can't wrap around and only have to make
		 * sure that there's enough space for the entire payload.
		 */
		if (wptr + total_size > rptr) {
			ret = -ENOMEM;
			goto out;
		}

		hdr = ep->txbfr.buf + wptr;
		wptr += sizeof(*hdr);
	} else {
		/* We need enough space to place at least a queue entry */
		if (wptr + sizeof(*hdr) > ep->txbfr.bufsz) {
			ret = -ENOMEM;
			goto out;
		}

		/*
		 * If we can place a single queue entry but not the full payload
		 * we need to place one queue entry at the end of the ring
		 * buffer and then another one together with the entire
		 * payload at the beginning.
		 */
		if (wptr + total_size > ep->txbfr.bufsz) {
			/*
			 * Ensure there's space for the  queue entry at the
			 * beginning
			 */
			if (sizeof(*hdr) > rptr) {
				ret = -ENOMEM;
				goto out;
			}

			/*
			 * Place two queue entries to indicate we want to wrap
			 * around to the firmware.
			 */
			hdr = ep->txbfr.buf + wptr;
			hdr2 = ep->txbfr.buf;
			wptr = sizeof(*hdr);

			/* Ensure there's enough space for the entire payload */
			if (wptr + total_epic_size > rptr) {
				ret = -ENOMEM;
				goto out;
			}
		} else {
			/* We have enough space to place the entire payload */
			hdr = ep->txbfr.buf + wptr;
			wptr += sizeof(*hdr);
		}
	}
	/*
	 * At this point we're guaranteed that hdr (and possibly hdr2) point
	 * to a buffer large enough to fit the queue entry and that we have
	 * enough space at wptr to store the payload.
	 */

	hdr->magic = cpu_to_le32(QE_MAGIC);
	hdr->size = cpu_to_le32(total_epic_size);
	hdr->channel = cpu_to_le32((compact || ep->wire_v2) ? 0 : channel);
	hdr->type = cpu_to_le32(ep->wire_v2 ? 0 : etype);
	if (hdr2)
		memcpy(hdr2, hdr, sizeof(*hdr));

	if (ep->wire_v2) {
		size_t written;

		ret = afkv2_encode_frame(ep->txbfr.buf + wptr, total_epic_size,
					 ep->qe_seq++, channel, token, stype, v2_category,
					 v2_flags, payload, payload_len, &written);
		if (ret)
			goto out;
		wptr += written;
	} else if (compact) {
		u8 *wire = ep->txbfr.buf + wptr;

		/* Experimental symmetry with the published H17P receive layout. */
		memset(wire, 0, sizeof(*ehdr) + sizeof(struct epic_sub_hdr_h17p));
		wire[0] = ep->qe_seq++;
		put_unaligned_le16(channel, wire + 2);
		put_unaligned_le32(total_epic_size - 8, wire + 4);
		wire[16] = stype;
		wire[17] = ecat;
		put_unaligned_le32(tag, wire + 20);
		wptr += sizeof(*ehdr) + sizeof(struct epic_sub_hdr_h17p);
	} else {
		ehdr = ep->txbfr.buf + wptr;
		memset(ehdr, 0, sizeof(*ehdr));
		ehdr->version = 2;
		ehdr->seq = cpu_to_le16(ep->qe_seq++);
		ehdr->timestamp = cpu_to_le64(0);
		wptr += sizeof(*ehdr);

		eshdr = ep->txbfr.buf + wptr;
		memset(eshdr, 0, sizeof(*eshdr));
		eshdr->length = cpu_to_le32(payload_len);
		eshdr->version = 4;
		eshdr->category = ecat;
		eshdr->type = cpu_to_le16(stype);
		eshdr->timestamp = cpu_to_le64(0);
		eshdr->tag = cpu_to_le16(tag);
		if (ecat == EPIC_CAT_REPLY)
			eshdr->inline_len = cpu_to_le32(payload_len - 4);
		else
			eshdr->inline_len = cpu_to_le32(0);
		wptr += sizeof(*eshdr);
	}

	if (!ep->wire_v2) {
		memcpy(ep->txbfr.buf + wptr, payload, payload_len);
		wptr += payload_len;
	}
	wptr = afk_ring_advance(wptr, 0, ep->txbfr.bufsz,
				ep->txbfr.stride);
	trace_afk_send_rwptr_post(ep, rptr, wptr);

	/* Publish the completed entry before allowing the consumer to read it. */
	dma_wmb();
	*ep->txbfr.wptr = cpu_to_le32(wptr);
	afk_send(ep, FIELD_PREP(RBEP_TYPE, RBEP_SEND) |
			     FIELD_PREP(SEND_WPTR, wptr));
	ret = 0;

out:
	spin_unlock_irqrestore(&ep->lock, flags);
	return ret;
}

int afk_send_epic(struct apple_dcp_afkep *ep, u32 channel, u16 tag,
		  enum epic_type etype, enum epic_category ecat, u8 stype,
		  const void *payload, size_t payload_len)
{
	return afk_send_epic_token(ep, channel, tag, etype, ecat, stype,
				   payload, payload_len, 0);
}

int afk_send_command(struct apple_epic_service *service, u8 type,
		     const void *payload, size_t payload_len, void *output,
		     size_t output_len, u32 *retcode)
{
	return afk_send_command_timeout(service, type, payload, payload_len,
					output, output_len, retcode,
					MSEC_PER_SEC);
}

int afk_send_command_timeout(struct apple_epic_service *service, u8 type,
			     const void *payload, size_t payload_len,
			     void *output, size_t output_len, u32 *retcode,
			     unsigned int timeout_ms)
{
	struct epic_cmd cmd;
	void *rxbuf, *txbuf;
	dma_addr_t rxbuf_dma, txbuf_dma;
	unsigned long flags;
	int ret, idx;
	u16 tag;
	struct apple_dcp_afkep *ep;
	DECLARE_COMPLETION_ONSTACK(completion);

	service = afk_service_get(service);
	if (!service)
		return -ENODEV;
	ep = service->ep;
	if (READ_ONCE(ep->dcp->quiescing)) {
		ret = -ENODEV;
		goto err_put_service;
	}

	if (ep->wire_v2 && (payload_len > SZ_1M || output_len > SZ_1M ||
			    (payload_len && !payload))) {
		ret = -EINVAL;
		goto err_put_service;
	}

	rxbuf = dma_alloc_coherent(ep->dcp->dev, output_len, &rxbuf_dma,
				   GFP_KERNEL);
	if (!rxbuf) {
		ret = -ENOMEM;
		goto err_put_service;
	}
	txbuf = dma_alloc_coherent(ep->dcp->dev, payload_len, &txbuf_dma,
				   GFP_KERNEL);
	if (!txbuf) {
		ret = -ENOMEM;
		goto err_free_rxbuf;
	}

	memcpy(txbuf, payload, payload_len);

	memset(&cmd, 0, sizeof(cmd));
	cmd.retcode = cpu_to_le32(0);
	cmd.rxbuf = cpu_to_le64(rxbuf_dma);
	cmd.rxlen = cpu_to_le32(output_len);
	cmd.txbuf = cpu_to_le64(txbuf_dma);
	cmd.txlen = cpu_to_le32(payload_len);

	spin_lock_irqsave(&service->lock, flags);
	if (service->torndown || !service->enabled) {
		ret = -ENODEV;
		goto err_unlock;
	}
	idx = bitmap_find_free_region(service->cmd_map, MAX_PENDING_CMDS, 0);
	if (idx < 0) {
		ret = -ENOSPC;
		goto err_unlock;
	}

	tag = ep->wire_v2 ? afkv2_command_id(service->cmd_tag, idx) :
		((service->cmd_tag & 0xff) << 8) | idx;
	service->cmd_tag++;

	service->cmds[idx].tag = tag;
	service->cmds[idx].type = type;
	service->cmds[idx].reply_length = 0;
	service->cmds[idx].rxbuf = rxbuf;
	service->cmds[idx].txbuf = txbuf;
	service->cmds[idx].rxbuf_dma = rxbuf_dma;
	service->cmds[idx].txbuf_dma = txbuf_dma;
	service->cmds[idx].rxlen = output_len;
	service->cmds[idx].txlen = payload_len;
	service->cmds[idx].free_on_ack = false;
	service->cmds[idx].done = false;
	service->cmds[idx].completion = &completion;
	init_completion(&completion);

	ret = afk_send_epic(service->ep, service->channel, tag,
			    EPIC_TYPE_COMMAND, EPIC_CAT_COMMAND, type, &cmd,
			    sizeof(cmd));
	spin_unlock_irqrestore(&service->lock, flags);
	if (ret)
		goto err_free_cmd;

	ret = wait_for_completion_timeout(&completion,
					  msecs_to_jiffies(timeout_ms));

	if (ret <= 0) {
		spin_lock_irqsave(&service->lock, flags);
		/*
		 * Check again while we're inside the lock to make sure
		 * the command wasn't completed just after
		 * wait_for_completion_timeout returned.
		 */
		if (!service->cmds[idx].done) {
			service->cmds[idx].completion = NULL;
			service->cmds[idx].free_on_ack = true;
			spin_unlock_irqrestore(&service->lock, flags);
			dev_warn(ep->dcp->dev,
				 "AFK[ep:%02x]: %s (chan:%u) command type 0x%x tag 0x%04x timed out after %u ms\n",
				 ep->endpoint, service->ops->name, service->channel,
				 type, tag, timeout_ms);
			afk_service_put(service);
			return -ETIMEDOUT;
		}
		spin_unlock_irqrestore(&service->lock, flags);
	}

	ret = 0;
	if (retcode)
		*retcode = service->cmds[idx].retcode;
	if (ep->wire_v2 && !service->cmds[idx].retcode &&
	    service->cmds[idx].reply_length < output_len)
		ret = -EMSGSIZE;
	else if (output && output_len && (!ep->wire_v2 || !service->cmds[idx].retcode)) {
		if (ep->wire_v2)
			dma_rmb();
		memcpy(output, rxbuf, output_len);
	}

err_free_cmd:
	spin_lock_irqsave(&service->lock, flags);
	service->cmds[idx].completion = NULL;
	bitmap_release_region(service->cmd_map, idx, 0);
err_unlock:
	spin_unlock_irqrestore(&service->lock, flags);
	dma_free_coherent(ep->dcp->dev, payload_len, txbuf, txbuf_dma);
err_free_rxbuf:
	dma_free_coherent(ep->dcp->dev, output_len, rxbuf, rxbuf_dma);
err_put_service:
	afk_service_put(service);
	return ret;
}

int afk_service_call(struct apple_epic_service *service, u16 group, u32 command,
		     const void *data, size_t data_len, size_t data_pad,
		     void *output, size_t output_len, size_t output_pad)
{
	return afk_service_call_timeout(service, group, command, data, data_len,
					data_pad, output, output_len,
					output_pad, MSEC_PER_SEC);
}

int afk_service_call_timeout(struct apple_epic_service *service, u16 group,
			     u32 command, const void *data, size_t data_len,
			     size_t data_pad, void *output, size_t output_len,
			     size_t output_pad, unsigned int timeout_ms)
{
	struct epic_service_call *call;
	void *bfr;
	size_t bfr_len = max(data_len + data_pad, output_len + output_pad) +
			 sizeof(*call);
	int ret;
	u32 retcode;
	u32 retlen;

	if (!service)
		return -ENODEV;

	bfr = kzalloc(bfr_len, GFP_KERNEL);
	if (!bfr)
		return -ENOMEM;

	call = bfr;

	memset(call, 0, sizeof(*call));
	call->group = cpu_to_le16(group);
	call->command = cpu_to_le32(command);
	call->data_len = cpu_to_le32(data_len + data_pad);
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);

	memcpy(bfr + sizeof(*call), data, data_len);

	ret = afk_send_command_timeout(service, EPIC_SUBTYPE_STD_SERVICE, bfr,
				       bfr_len, bfr, bfr_len, &retcode,
				       timeout_ms);
	if (ret) {
		dev_warn(service->ep->dcp->dev,
			 "AFK[ep:%02x]: %s (chan:%u) service call group %u cmd %u failed to complete: %d\n",
			 service->ep->endpoint, service->ops->name,
			 service->channel, group, command, ret);
		goto out;
	}
	if (retcode) {
		dev_warn(service->ep->dcp->dev,
			 "AFK[ep:%02x]: %s (chan:%u) service call group %u cmd %u returned retcode 0x%08x (reply data_len %u)\n",
			 service->ep->endpoint, service->ops->name,
			 service->channel, group, command, retcode,
			 le32_to_cpu(call->data_len));
		ret = -EINVAL;
		goto out;
	}
	if (le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC ||
	    le16_to_cpu(call->group) != group ||
	    le32_to_cpu(call->command) != command) {
		dev_warn(service->ep->dcp->dev,
			 "AFK[ep:%02x]: %s (chan:%u) service call reply mismatch: sent magic 0x%08x group %u cmd %u, got magic 0x%08x group %u cmd %u\n",
			 service->ep->endpoint, service->ops->name,
			 service->channel, EPIC_SERVICE_CALL_MAGIC, group,
			 command, le32_to_cpu(call->magic),
			 le16_to_cpu(call->group), le32_to_cpu(call->command));
		ret = -EINVAL;
		goto out;
	}
	dev_dbg(service->ep->dcp->dev,
		"AFK[ep:%02x]: %s (chan:%u) service call group %u cmd %u ok (reply data_len %u)\n",
		service->ep->endpoint, service->ops->name, service->channel,
		group, command, le32_to_cpu(call->data_len));

	retlen = le32_to_cpu(call->data_len);
	if (output_len < retlen)
		retlen = output_len;
	if (output && output_len) {
		memset(output, 0, output_len);
		memcpy(output, bfr + sizeof(*call), retlen);
	}

out:
	kfree(bfr);
	return ret;
}

#if IS_ENABLED(CONFIG_DRM_APPLE_DEBUG)

#define AFK_DEBUGFS_MAX_REPLY 8192

static ssize_t service_call_write_file(struct file *file, const char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct apple_epic_service *srv = file->private_data;
	void *buf;
	int ret;
	struct {
		u32 group;
		u32 command;
	} call_info;

	if (count < sizeof(call_info))
		return -EINVAL;
	if (!srv->debugfs.scratch) {
		srv->debugfs.scratch = \
			devm_kzalloc(srv->ep->dcp->dev, AFK_DEBUGFS_MAX_REPLY, GFP_KERNEL);
		if (!srv->debugfs.scratch)
			return -ENOMEM;
	}

	if (copy_from_user(&call_info, user_buf, sizeof(call_info)))
		return -EFAULT;
	user_buf += sizeof(call_info);
	count -= sizeof(call_info);

	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (copy_from_user(buf, user_buf, count)) {
		kfree(buf);
		return -EFAULT;
	}

	memset(srv->debugfs.scratch, 0, AFK_DEBUGFS_MAX_REPLY);
	dma_mb();

	ret = afk_service_call(srv, call_info.group, call_info.command, buf, count, 0,
			       srv->debugfs.scratch, AFK_DEBUGFS_MAX_REPLY, 0);
	kfree(buf);

	if (ret < 0)
		return ret;

	return count + sizeof(call_info);
}

static ssize_t service_call_read_file(struct file *file, char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct apple_epic_service *srv = file->private_data;

	if (!srv->debugfs.scratch)
		return -EINVAL;

	return simple_read_from_buffer(user_buf, count, ppos,
				       srv->debugfs.scratch, AFK_DEBUGFS_MAX_REPLY);
}

static const struct file_operations service_call_fops = {
	.open = simple_open,
	.write = service_call_write_file,
	.read = service_call_read_file,
};

static ssize_t service_raw_call_write_file(struct file *file, const char __user *user_buf,
					   size_t count, loff_t *ppos)
{
	struct apple_epic_service *srv = file->private_data;
	u32 retcode;
	int ret;

	if (!count)
		return 0;
	if (count > AFK_DEBUGFS_MAX_REPLY)
		return -E2BIG;

	if (!srv->debugfs.scratch) {
		srv->debugfs.scratch = \
			devm_kzalloc(srv->ep->dcp->dev, AFK_DEBUGFS_MAX_REPLY, GFP_KERNEL);
		if (!srv->debugfs.scratch)
			return -ENOMEM;
	}

	memset(srv->debugfs.scratch, 0, AFK_DEBUGFS_MAX_REPLY);
	if (copy_from_user(srv->debugfs.scratch, user_buf, count))
		return -EFAULT;

	ret = afk_send_command(srv, EPIC_SUBTYPE_STD_SERVICE, srv->debugfs.scratch, count,
			       srv->debugfs.scratch, AFK_DEBUGFS_MAX_REPLY, &retcode);
	if (ret < 0)
		return ret;
	if (retcode)
		return -EINVAL;

	return count;
}

static ssize_t service_raw_call_read_file(struct file *file, char __user *user_buf,
					  size_t count, loff_t *ppos)
{
	struct apple_epic_service *srv = file->private_data;

	if (!srv->debugfs.scratch)
		return -EINVAL;

	return simple_read_from_buffer(user_buf, count, ppos,
				       srv->debugfs.scratch, AFK_DEBUGFS_MAX_REPLY);
}

static const struct file_operations service_raw_call_fops = {
	.open = simple_open,
	.write = service_raw_call_write_file,
	.read = service_raw_call_read_file,
};

static void afk_populate_service_debugfs(struct apple_epic_service *srv)
{
	if (!srv->ep->debugfs_entry || !srv->ops)
		return;

	if (strcmp(srv->ops->name, "DCPAVAudioInterface") == 0) {
		srv->debugfs.entry = debugfs_create_dir(srv->ops->name,
							srv->ep->debugfs_entry);
		debugfs_create_file("call", 0600, srv->debugfs.entry, srv,
				&service_call_fops);
		debugfs_create_file("raw_call", 0600, srv->debugfs.entry, srv,
				&service_raw_call_fops);
	}
}

static void afk_remove_service_debugfs(struct apple_epic_service *srv)
{
	if (srv->debugfs.entry) {
		debugfs_remove_recursive(srv->debugfs.entry);
		srv->debugfs.entry = NULL;
	}
}

#endif
