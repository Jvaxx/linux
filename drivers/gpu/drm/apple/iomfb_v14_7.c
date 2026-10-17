// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

/*
 * DCP firmware 14.7.0 (J514S internal panel) instantiation of the IOMFB
 * template.  The method and callback numbers and the exact callback shapes
 * are those of the 14.7.0 image the J514S boot loader runs (UUID
 * DDF38191-93B3-324A-BC8F-643006F5AC82).
 *
 * The session is adopted running: READY, then A401 -> A455 with the
 * callbacks below, and no modeset.  Every callback the session handles has
 * an exact shape and a fixed answer; anything else fails the session (see
 * dcpep_handle_cb_v14_7).
 */

#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/log2.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <drm/drm_modes.h>

#include "dcp-property-store.h"
#include "iomfb_v12_3.h"
#include "iomfb_v13_3.h"
#include "iomfb_v14_7.h"
#include "iomfb_h17p.h"
#include "version_utils.h"

/*
 * 14.7 method numbers (in/out bytes).  The J514S panel lifecycle sends only
 * A401, A455, A406, A407 and, nested in callbacks, A444, A029, A466, A000,
 * A463, A374, A131, A132 and A410.
 * The other entries name the 14.7 numbers of methods the common template
 * knows, for identification.  The common 13.5 shapes fit, except:
 * last_client_close (A456, 4/4) and abort_swaps_dcp (A467, 8/4, a scalar
 * client handle) are left out because the J514S lifecycle has no client
 * teardown, and set_parameter_dcp (A440, 40/4 with four u64 values) because
 * the common adaptive-sync request is u32-valued.
 */
static const struct dcp_method_entry dcp_methods[dcpep_num_methods] = {
	IOMFB_METHOD("A000", dcpep_late_init_signal),	/* 4/4, in true */
	IOMFB_METHOD("A029", dcpep_setup_video_limits),	/* 0/0 */
	IOMFB_METHOD("A131", iomfbep_a131_pmu_service_matched), /* 0/4, in D206 */
	IOMFB_METHOD("A132", iomfbep_a132_backlight_service_matched), /* 0/4, in D207 */
	IOMFB_METHOD("A377", dcpep_set_create_dfb),	/* 0/0, optional in D121 */
	/*
	 * The native D100 continuation is A378 (vi_set_temperature_hint);
	 * this session sends A374, the 0/4 headless query, which is what the
	 * panel is qualified with.
	 */
	IOMFB_METHOD("A374", iomfbep_a358_vi_set_temperature_hint),
	IOMFB_METHOD("A401", dcpep_start_signal),	/* 0/4, 1 */
	IOMFB_METHOD("A406", dcpep_swap_start),		/* 16/8 */
	IOMFB_METHOD("A407", dcpep_swap_submit),	/* 0x1b58/12 */
	IOMFB_METHOD("A409", dcpep_set_display_device),	/* 4/4 */
	IOMFB_METHOD("A410", dcpep_is_main_display),	/* 0/4 */
	IOMFB_METHOD("A411", dcpep_set_digital_out_mode), /* 8/4 */
	IOMFB_METHOD("A421", iomfbep_set_matrix),	/* 80/4 */
	IOMFB_METHOD("A425", iomfbep_get_color_remap_mode), /* 8/8 */
	IOMFB_METHOD("A444", dcpep_create_default_fb),	/* 0/4, in D121 */
	IOMFB_METHOD("A448", dcpep_enable_disable_video_power_savings), /* 4/4 */
	IOMFB_METHOD("A455", dcpep_first_client_open),	/* 0/0 */
	IOMFB_METHOD("A463", dcpep_set_display_refresh_properties), /* 0/4 */
	IOMFB_METHOD("A466", dcpep_flush_supports_power), /* 4/0, in true */
	IOMFB_METHOD("A472", dcpep_set_power_state),	/* 12/8 */
};

#define DCP_FW v14_7
#define DCP_FW_VER DCP_FW_VERSION(14, 7, 0)

#include "iomfb_template.c"

/*
 * Exact 14.7 callback shapes (input/output bytes) of the callbacks the
 * J514S session handles.
 * Unlike the 13.x variants, a 14.7 callback whose number or shape is not
 * listed here is a protocol error: the session is latched failed, never
 * acknowledged with an invented reply.
 */
struct dcp_callback_shape_v14_7 {
	u16 in_len;
	u16 out_len;
	bool valid;
};

#define DCP_CB_V14_7(in, out) { .in_len = (in), .out_len = (out), .valid = true }

static const struct dcp_callback_shape_v14_7 callback_shapes[IOMFB_MAX_CB] = {
	[0] = DCP_CB_V14_7(0, 4),		/* did_boot_signal */
	[1] = DCP_CB_V14_7(0, 4),		/* did_power_on_signal */
	[3] = DCP_CB_V14_7(4, 60),		/* rt_bandwidth_setup_ap */
	[6] = DCP_CB_V14_7(60, 56),		/* set_frame_sync_props */
	[100] = DCP_CB_V14_7(0, 0),		/* match_pmu_service */
	[101] = DCP_CB_V14_7(0, 4),		/* get_display_default_stride */
	[102] = DCP_CB_V14_7(68, 0),		/* set_number_property */
	[104] = DCP_CB_V14_7(68, 0),		/* set_boolean_property */
	[107] = DCP_CB_V14_7(64, 0),		/* remove_property */
	[108] = DCP_CB_V14_7(0, 4),		/* create_provider_service */
	[109] = DCP_CB_V14_7(0, 4),		/* create_product_service */
	[110] = DCP_CB_V14_7(0, 4),		/* create_pmu_service */
	[111] = DCP_CB_V14_7(0, 4),		/* create_iomfb_service */
	[112] = DCP_CB_V14_7(0, 4),		/* create_backlight_service */
	[113] = DCP_CB_V14_7(0, 4),		/* create_nvram_service */
	[114] = DCP_CB_V14_7(4164, 4100),	/* CoreAnalyticsSendEvent */
	[115] = DCP_CB_V14_7(16, 8),		/* get_tiling_state */
	[116] = DCP_CB_V14_7(16, 4),		/* set_tiling_state */
	[121] = DCP_CB_V14_7(0, 4),		/* start_hardware_boot */
	[122] = DCP_CB_V14_7(0, 4),		/* is_dark_boot */
	[123] = DCP_CB_V14_7(0, 4),		/* is_waking_from_hibernate */
	[125] = DCP_CB_V14_7(100, 36),		/* read_edt_data */
	[127] = DCP_CB_V14_7(4, 4),		/* setDCPAVPropStart */
	[128] = DCP_CB_V14_7(4104, 4),		/* setDCPAVPropChunk */
	[129] = DCP_CB_V14_7(64, 4),		/* setDCPAVPropEnd */
	[201] = DCP_CB_V14_7(12, 20),		/* map_buf */
	[206] = DCP_CB_V14_7(0, 4),		/* match_pmu_service_2 */
	[207] = DCP_CB_V14_7(0, 4),		/* match_backlight_service */
	[209] = DCP_CB_V14_7(0, 8),		/* get_calendar_time_ms */
	[300] = DCP_CB_V14_7(16, 0),		/* pr_publish */
	[400] = DCP_CB_V14_7(76, 3076),		/* get_property (provider) */
	[401] = DCP_CB_V14_7(80, 12),		/* get_uint_prop */
	[406] = DCP_CB_V14_7(72, 0),		/* set_fx_prop */
	[408] = DCP_CB_V14_7(8, 8),		/* getClockFrequency */
	[413] = DCP_CB_V14_7(4168, 4),		/* sr_set_property_dict */
	[414] = DCP_CB_V14_7(80, 4),		/* sr_set_property_int */
	[415] = DCP_CB_V14_7(76, 4),		/* sr_set_property_bool */
	[451] = DCP_CB_V14_7(20, 28),		/* allocate_buffer */
	[454] = DCP_CB_V14_7(4, 4),		/* release_mem_desc */
	[552] = DCP_CB_V14_7(4164, 4),		/* set_property_dict_0 */
	[561] = DCP_CB_V14_7(4164, 4),		/* set_property_dict */
	[563] = DCP_CB_V14_7(76, 4),		/* set_property_int */
	[565] = DCP_CB_V14_7(72, 4),		/* set_property_bool */
	[567] = DCP_CB_V14_7(128, 4),		/* set_property_str */
	[576] = DCP_CB_V14_7(88, 76),		/* hotplug_notify_gated */
	[577] = DCP_CB_V14_7(4, 0),		/* powerstate_notify */
	[582] = DCP_CB_V14_7(8, 4),		/* create_default_fb_surface */
	[588] = DCP_CB_V14_7(8, 0),		/* update_ap_hdcp_encryption_status */
	[589] = DCP_CB_V14_7(0x6f0, 0),		/* swap_complete_ap_gated */
	[596] = DCP_CB_V14_7(0, 4),		/* isDFBAllocated */
	[598] = DCP_CB_V14_7(0, 0),		/* find_swap_function_gated */
	/*
	 * Not in this image's callback registration; answered with a nested
	 * A410 should the firmware send it.
	 */
	[599] = DCP_CB_V14_7(0, 0),
};

bool iomfb_check_callback_v14_7(int tag, u32 in_len, u32 out_len)
{
	const struct dcp_callback_shape_v14_7 *shape;

	if (tag < 0 || tag >= ARRAY_SIZE(callback_shapes))
		return false;
	shape = &callback_shapes[tag];
	return shape->valid && shape->in_len == in_len &&
	       shape->out_len == out_len;
}

/*
 * 14.7 callback handlers.  A malformed or unsupported request fails the
 * session instead of being answered (no ACK, see iomfb_v14_7_fail()).
 */
static bool v14_7_reject(struct apple_dcp *dcp, int tag, const char *what)
{
	dev_err(dcp->dev, "D%03d rejected: %s\n", tag, what);
	iomfb_v14_7_fail(dcp, "callback rejected");
	return false;
}

/* D003: no real-time bandwidth service; zero configuration, status 1. */
static bool dcpep_cb_rt_bandwidth_v14_7(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);
	put_unaligned_le32(1, (u8 *)out + 56);
	return true;
}

/* D006: return the 56-byte frame-sync object unchanged when present. */
static bool dcpep_cb_frame_sync_props_v14_7(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	const u8 *req = in;

	trace_iomfb_callback(dcp, tag, __func__);
	if (!(req[56] & 1))
		memcpy(out, req, 56);
	return true;
}

/* D101: the stride of the inherited boot framebuffer. */
static bool dcpep_cb_default_stride_v14_7(struct apple_dcp *dcp, int tag,
					  void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);
	put_unaligned_le32(dcp->v14_7->boot_stride, out);
	return true;
}

/*
 * Property setters (D102/D104/D107, D413-D415, D552/D561/D563/D565/D567):
 * the registry keeps what the firmware sets, for its later getters.
 */
static bool dcpep_cb_set_property_v14_7(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	struct dcp_property_store *store = dcp->v14_7->props;
	const u8 *req = in;
	u32 service = 0, off = 0;
	const char *key;
	int ret;

	trace_iomfb_callback(dcp, tag, __func__);
	if (tag == 413 || tag == 414 || tag == 415) {
		service = get_unaligned_le32(req);
		off = 4;
	}
	key = (const char *)req + off;
	if (dcp_property_key_valid(key))
		return v14_7_reject(dcp, tag, "unterminated key");

	switch (tag) {
	case 102:
		ret = dcp_property_set_scalar(store, 0, key, get_unaligned_le32(req + 64));
		break;
	case 104:
		ret = dcp_property_set_scalar(store, 0, key, !!req[64]);
		break;
	case 107:
		ret = dcp_property_remove_scalar(store, 0, key);
		break;
	case 413:
	case 552:
	case 561:
		ret = (req[off + 4160] & 1) ? 0 :
		      dcp_property_set_raw(store, service, key, req + off + 64, 4096);
		break;
	case 414:
	case 563:
		ret = (req[off + 72] & 1) ? 0 :
		      dcp_property_set_scalar(store, service, key,
					      get_unaligned_le64(req + off + 64));
		break;
	case 415:
	case 565:
		ret = (req[off + 68] & 1) ? 0 :
		      dcp_property_set_scalar(store, service, key,
					      get_unaligned_le32(req + off + 64));
		break;
	case 567:
		if (dcp_property_key_valid(req + 64))
			return v14_7_reject(dcp, tag, "unterminated string value");
		ret = dcp_property_set_raw(store, 0, key, req + 64,
					   strlen((const char *)req + 64));
		break;
	default:
		return v14_7_reject(dcp, tag, "not a property setter");
	}
	if (ret)
		return v14_7_reject(dcp, tag, "property not stored");

	/* The OSString/C-string setters report acceptance. */
	if (tag != 102 && tag != 104 && tag != 107)
		put_unaligned_le32(1, out);
	return true;
}

/* D400: raw property lookup; length 0 when absent. */
static bool dcpep_cb_get_property_v14_7(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	const u8 *req = in;
	const char *key = (const char *)req + 4;
	const void *data;
	u32 size;

	trace_iomfb_callback(dcp, tag, __func__);
	if (dcp_property_key_valid(key) || (req[72] & 1))
		return v14_7_reject(dcp, tag, "malformed request");
	data = dcp_property_get_raw(dcp->v14_7->props, get_unaligned_le32(req),
				    key, &size);
	if (data) {
		if (size > 0xc00 || size > get_unaligned_le32(req + 68))
			return v14_7_reject(dcp, tag, "property larger than the buffer");
		if (size)
			memcpy(out, data, size);
		put_unaligned_le32(size, (u8 *)out + 0xc00);
	}
	return true;
}

/*
 * D401: numeric property lookup, found=false when absent.  The mini-LED
 * PMU "Temperature" answer is a fixed compatibility value, never a sensor.
 */
static bool dcpep_cb_get_uint_prop_v14_7(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	const u8 *req = in;
	const char *key = (const char *)req + 4;
	u64 value;

	trace_iomfb_callback(dcp, tag, __func__);
	if (dcp_property_key_valid(key))
		return v14_7_reject(dcp, tag, "unterminated key");
	if (dcp->panel.has_mini_led && !memcmp(req, "SUMP", 4) &&
	    !strcmp(key, "Temperature")) {
		put_unaligned_le64(3029, out);
		((u8 *)out)[8] = 1;
		return true;
	}
	if (!dcp_property_get_scalar(dcp->v14_7->props, get_unaligned_le32(req),
				     key, &value)) {
		put_unaligned_le64(value, out);
		((u8 *)out)[8] = 1;
	}
	return true;
}

/* D114: CoreAnalyticsSendEvent; return the event dictionary, status 0. */
static bool dcpep_cb_analytics_v14_7(struct apple_dcp *dcp, int tag,
				     void *out, void *in)
{
	const u8 *req = in;

	trace_iomfb_callback(dcp, tag, __func__);
	if (!memchr(req, 0, 64) || req[4160] > 1 || (!req[4160] && req[64] != 'd'))
		return v14_7_reject(dcp, tag, "malformed event");
	dev_dbg(dcp->dev, "DCP event %s\n", (const char *)req);
	if (!(req[4160] & 1))
		memcpy(out, req + 64, 4096);
	return true;
}

/* D115: no tiling; value 0, true. */
static bool dcpep_cb_get_tiling_state_v14_7(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	const u8 *req = in;

	trace_iomfb_callback(dcp, tag, __func__);
	if (req[12] > 1)
		return v14_7_reject(dcp, tag, "malformed request");
	put_unaligned_le32(1, (u8 *)out + 4);
	return true;
}

/* D125: return the requested default values unchanged, false. */
static bool dcpep_cb_read_edt_data_v14_7(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	const u8 *req = in;
	u32 count = get_unaligned_le32(req + 64);

	trace_iomfb_callback(dcp, tag, __func__);
	if (count > 8)
		return v14_7_reject(dcp, tag, "too many values");
	memcpy(out, req + 68, count * 4);
	return true;
}

/* D127/D128: chunked DCPAV property transfer into the registry. */
static bool dcpep_cb_prop_start_v14_7(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);
	if (dcp_property_chunk_start(dcp->v14_7->props, get_unaligned_le32(in)))
		return v14_7_reject(dcp, tag, "transfer not accepted");
	put_unaligned_le32(1, out);
	return true;
}

static bool dcpep_cb_prop_chunk_v14_7(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	const u8 *req = in;

	trace_iomfb_callback(dcp, tag, __func__);
	if (dcp_property_chunk_add(dcp->v14_7->props, req,
				   get_unaligned_le32(req + 4096),
				   get_unaligned_le32(req + 4100)))
		return v14_7_reject(dcp, tag, "chunk out of sequence");
	put_unaligned_le32(1, out);
	return true;
}

/*
 * The panel's mode comes from PreferredTimingElements, through the common
 * parser: the full panel (no notch crop) at its preferred timing.  This
 * session sets up no adaptive sync, so the mode is fixed-rate and presents
 * carry no timestamps.
 */
static int v14_7_preferred_timing(struct apple_dcp *dcp, const void *blob, u32 size)
{
	struct dcp_display_mode *modes, *preferred = NULL;
	struct dcp_parse_ctx ctx;
	unsigned int count = 0, i;
	unsigned long flags;
	int ret, refresh;

	ret = parse(blob, size, &ctx);
	if (ret)
		return ret;
	ctx.dcp = dcp;
	modes = enumerate_modes(&ctx, &count, dcp->panel.width_mm,
				dcp->panel.height_mm, 0, true, false);
	if (IS_ERR(modes))
		return PTR_ERR(modes);

	for (i = 0; i < count; i++) {
		modes[i].vrr = false;
		modes[i].min_vrr = 0;
		modes[i].max_vrr = 0;
		if (modes[i].mode.type & DRM_MODE_TYPE_PREFERRED)
			preferred = &modes[i];
	}
	refresh = preferred ? drm_mode_vrefresh(&preferred->mode) : 0;
	if (!preferred || preferred->mode.hdisplay != J514S_PANEL_WIDTH ||
	    preferred->mode.vdisplay != J514S_PANEL_HEIGHT ||
	    refresh < 59 || refresh > 121) {
		kfree(modes);
		return -EINVAL;
	}

	/* Mode readers may still hold the previous array; it is retained. */
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	dcp->modes = modes;
	dcp->nr_modes = count;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);

	dev_info(dcp->dev, "preferred timing %u colour %u: " DRM_MODE_FMT "\n",
		 preferred->timing_mode_id, preferred->color_mode_id,
		 DRM_MODE_ARG(&preferred->mode));
	return 0;
}

/* D129: the completed transfer becomes a named property. */
static bool dcpep_cb_prop_end_v14_7(struct apple_dcp *dcp, int tag,
				    void *out, void *in)
{
	struct dcp_v14_7_session *s = dcp->v14_7;
	const char *key = in;
	const void *blob;
	u32 size;

	trace_iomfb_callback(dcp, tag, __func__);
	if (dcp_property_key_valid(key) || dcp_property_chunk_end(s->props, key))
		return v14_7_reject(dcp, tag, "transfer incomplete");

	if (!strcmp(key, "PreferredTimingElements")) {
		blob = dcp_property_get_raw(s->props, 0, key, &size);
		if (!blob || v14_7_preferred_timing(dcp, blob, size))
			return v14_7_reject(dcp, tag, "preferred timing not admitted");
		s->modes_ready = true;
		iomfb_v14_7_try_activate(dcp);
	}
	put_unaligned_le32(1, out);
	return true;
}

/* D209 lives in the template; D300: runtime property publication. */
static bool dcpep_cb_pr_publish_v14_7(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	struct dcp_v14_7_session *s = dcp->v14_7;
	u32 id = get_unaligned_le32(in);
	u32 value = get_unaligned_le32((u8 *)in + 4);

	trace_iomfb_callback(dcp, tag, __func__);
	if (id == 19) {
		s->pr_nits = value;
		s->pr_nits_valid = true;
		trace_iomfb_brightness(dcp, value);
	} else if (id == 18) {
		s->pr_correction = value;
	}
	return true;
}

/* D408: the J514S boot ADT's VORP clocks: index 0 712 MHz, index 1 none. */
static bool dcpep_cb_get_frequency_v14_7(struct apple_dcp *dcp, int tag,
					 void *out, void *in)
{
	u32 index = get_unaligned_le32((u8 *)in + 4);

	trace_iomfb_callback(dcp, tag, __func__);
	if (memcmp(in, "VORP", 4) || index > 1)
		return v14_7_reject(dcp, tag, "unknown clock");
	put_unaligned_le64(index ? 0 : 712000000, out);
	return true;
}

static bool v14_7_dva_ok(u64 dva, u64 size)
{
	return dva >= DCP_V14_7_DVA_BASE && size &&
	       dva - DCP_V14_7_DVA_BASE < DCP_V14_7_DVA_SIZE &&
	       size <= DCP_V14_7_DVA_SIZE - (dva - DCP_V14_7_DVA_BASE);
}

/*
 * D451: zeroed, physically contiguous backing in the DCP's DMA window,
 * 16 KiB rounded, with its real physical address in the reply.  It is never
 * freed.
 */
static bool dcpep_cb_allocate_buffer_v14_7(struct apple_dcp *dcp, int tag,
					   void *out, void *in)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
	struct dcp_v14_7_session *s = dcp->v14_7;
	const u8 *req = in;
	u64 size = get_unaligned_le64(req + 4);
	u32 align = get_unaligned_le32(req + 12);
	struct dcp_v14_7_object *o;
	phys_addr_t pa;
	dma_addr_t dva;
	unsigned int n;
	u64 off;

	trace_iomfb_callback(dcp, tag, __func__);
	/* Byte 19 is padding, nonzero in otherwise valid requests. */
	if (get_unaligned_le32(req) != 0x703 || !size || size > DCP_V14_7_OBJECT_MAX ||
	    !align || !is_power_of_2(align) || align > SZ_16K ||
	    req[16] || req[17] || req[18])
		return v14_7_reject(dcp, tag, "unsupported allocation");
	size = ALIGN(size, SZ_16K);
	if (s->objects >= DCP_V14_7_OBJECTS ||
	    s->object_bytes > DCP_V14_7_OBJECT_TOTAL - size || !domain)
		return v14_7_reject(dcp, tag, "allocation limit");

	if (!dma_alloc_attrs(dcp->dev, size, &dva, GFP_KERNEL | __GFP_ZERO,
			     DMA_ATTR_FORCE_CONTIGUOUS))
		return v14_7_reject(dcp, tag, "out of memory");
	/* Retained from here on, whatever happens next. */
	pa = iommu_iova_to_phys(domain, dva);
	if (!pa)
		return v14_7_reject(dcp, tag, "allocation not mapped");
	for (off = 0; off < size; off += SZ_16K)
		if (iommu_iova_to_phys(domain, dva + off) != pa + off)
			return v14_7_reject(dcp, tag, "allocation not contiguous");
	if (((pa | dva) & (SZ_16K - 1)) || pa >= BIT_ULL(42) ||
	    size > BIT_ULL(42) - pa || !v14_7_dva_ok(dva, size))
		return v14_7_reject(dcp, tag, "allocation outside its window");
	for (n = 0; n < s->objects; n++) {
		o = &s->object[n];
		if ((dva < o->dva + o->size && o->dva < dva + size) ||
		    (pa < o->phys + o->size && o->phys < pa + size))
			return v14_7_reject(dcp, tag, "allocation overlaps");
	}

	o = &s->object[s->objects++];
	*o = (struct dcp_v14_7_object) { .phys = pa, .dva = dva, .size = size };
	s->object_bytes += size;

	put_unaligned_le64(pa, out);
	put_unaligned_le64(dva, (u8 *)out + 8);
	put_unaligned_le64(size, (u8 *)out + 16);
	put_unaligned_le32(s->objects, (u8 *)out + 24);
	return true;
}

/*
 * D201: map a live allocation at the same DVA for PIODMA (SID4).  The
 * firmware may ask again for an already mapped ID; that is answered with the
 * existing mapping.
 */
static bool dcpep_cb_map_piodma_v14_7(struct apple_dcp *dcp, int tag,
				      void *out, void *in)
{
	struct dcp_v14_7_session *s = dcp->v14_7;
	const u8 *req = in;
	u64 id = get_unaligned_le64(req);
	struct dcp_v14_7_object *o;
	u64 off;

	trace_iomfb_callback(dcp, tag, __func__);
	if (!id || id > s->objects || get_unaligned_le32(req + 8) != 1)
		return v14_7_reject(dcp, tag, "unknown buffer");
	o = &s->object[id - 1];
	if (o->retired)
		return v14_7_reject(dcp, tag, "released buffer");

	if (!o->mapped) {
		if (!dcp->iommu_dom ||
		    iommu_map(dcp->iommu_dom, o->dva, o->phys, o->size,
			      IOMMU_READ | IOMMU_WRITE, GFP_KERNEL))
			return v14_7_reject(dcp, tag, "PIODMA mapping failed");
		for (off = 0; off < o->size; off += SZ_16K)
			if (iommu_iova_to_phys(dcp->iommu_dom, o->dva + off) != o->phys + off)
				return v14_7_reject(dcp, tag, "PIODMA mapping mismatch");
		o->mapped = true;
	}

	/* AP address 0, the mapped DVA, status 0. */
	put_unaligned_le64(0, out);
	put_unaligned_le64(o->dva, (u8 *)out + 8);
	put_unaligned_le32(0, (u8 *)out + 16);
	return true;
}

/*
 * D454: the firmware drops a buffer.  No fence says the display engine is
 * done with it, so its backing and both translations stay until reboot.
 */
static bool dcpep_cb_release_mem_desc_v14_7(struct apple_dcp *dcp, int tag,
					    void *out, void *in)
{
	struct dcp_v14_7_session *s = dcp->v14_7;
	u32 id = get_unaligned_le32(in);

	trace_iomfb_callback(dcp, tag, __func__);
	if (!id || id > s->objects || s->object[id - 1].retired)
		return v14_7_reject(dcp, tag, "unknown buffer");
	s->object[id - 1].retired = true;
	put_unaligned_le32(1, out);
	return true;
}

/* D576: return the tiled-display block unchanged when present. */
static bool dcpep_cb_hotplug_v14_7(struct apple_dcp *dcp, int tag,
				   void *out, void *in)
{
	const u8 *req = in;

	trace_iomfb_callback(dcp, tag, __func__);
	dcp->v14_7->hotplug_flags = get_unaligned_le64(req);
	if (!(req[84] & 1))
		memcpy(out, req + 8, 76);
	return true;
}

/* D599: answered after a nested is_main_display (A410), which must be true. */
static void dcp_main_display_v14_7(struct apple_dcp *dcp, void *out, void *cookie)
{
	if (!out || get_unaligned_le32(out) != 1) {
		iomfb_v14_7_fail(dcp, "A410 is_main_display not true");
		return;
	}
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static bool dcpep_cb_main_display_v14_7(struct apple_dcp *dcp, int tag,
					void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);
	dcp_is_main_display(dcp, false, dcp_main_display_v14_7, NULL);
	return false;
}

bool iomfb_v14_7_needs_continuation(int tag)
{
	return tag == 100 || tag == 121 || tag == 206 || tag == 207 || tag == 599;
}

/*
 * 14.7 callback numbering: 13.5 plus one from D114 to D129 (D114 became
 * CoreAnalyticsSendEvent, D120 upload_trace_end), D583/D587 moved down by
 * one (D584 is swap_notify_gated, D588 the HDCP status), D589 unchanged.
 */
static const iomfb_cb_handler cb_handlers[IOMFB_MAX_CB] = {
	[0] = trampoline_true, /* did_boot_signal */
	[1] = trampoline_true, /* did_power_on_signal */
	[3] = dcpep_cb_rt_bandwidth_v14_7, /* declined, status 1 at +56 */
	[6] = dcpep_cb_frame_sync_props_v14_7,
	[100] = iomfbep_cb_match_pmu_service, /* nested A374 */
	[101] = dcpep_cb_default_stride_v14_7,
	[102] = dcpep_cb_set_property_v14_7, /* set_number_property */
	[104] = dcpep_cb_set_property_v14_7, /* set_boolean_property */
	[107] = dcpep_cb_set_property_v14_7, /* remove_property */
	[108] = trampoline_true, /* create_provider_service */
	[109] = trampoline_true, /* create_product_service */
	[110] = trampoline_true, /* create_pmu_service */
	[111] = trampoline_true, /* create_iomfb_service */
	[112] = trampoline_true, /* create_backlight_service */
	[113] = trampoline_true, /* create_nvram_service */
	[114] = dcpep_cb_analytics_v14_7, /* CoreAnalyticsSendEvent */
	[115] = dcpep_cb_get_tiling_state_v14_7,
	[116] = trampoline_false, /* set_tiling_state */
	[121] = dcpep_cb_boot_1, /* start_hardware_boot */
	[122] = trampoline_false, /* is_dark_boot */
	[123] = trampoline_false, /* is_waking_from_hibernate */
	[125] = dcpep_cb_read_edt_data_v14_7,
	[127] = dcpep_cb_prop_start_v14_7,
	[128] = dcpep_cb_prop_chunk_v14_7,
	[129] = dcpep_cb_prop_end_v14_7,
	[201] = dcpep_cb_map_piodma_v14_7,
	[206] = iomfbep_cb_match_pmu_service_2, /* nested A131 */
	[207] = iomfbep_cb_match_backlight_service, /* nested A132 */
	[209] = trampoline_get_time,
	[300] = dcpep_cb_pr_publish_v14_7,
	[400] = dcpep_cb_get_property_v14_7,
	[401] = dcpep_cb_get_uint_prop_v14_7,
	[406] = trampoline_set_fx_prop,
	[408] = dcpep_cb_get_frequency_v14_7,
	[413] = dcpep_cb_set_property_v14_7, /* dictionary */
	[414] = dcpep_cb_set_property_v14_7, /* number */
	[415] = dcpep_cb_set_property_v14_7, /* Boolean */
	[451] = dcpep_cb_allocate_buffer_v14_7,
	[454] = dcpep_cb_release_mem_desc_v14_7,
	[552] = dcpep_cb_set_property_v14_7, /* dictionary */
	[561] = dcpep_cb_set_property_v14_7, /* dictionary */
	[563] = dcpep_cb_set_property_v14_7, /* number */
	[565] = dcpep_cb_set_property_v14_7, /* Boolean */
	[567] = dcpep_cb_set_property_v14_7, /* string */
	[576] = dcpep_cb_hotplug_v14_7,
	[577] = trampoline_nop, /* powerstate_notify */
	[582] = trampoline_false, /* create_default_fb_surface */
	[588] = trampoline_nop, /* update_ap_hdcp_encryption_status */
	[589] = trampoline_swap_complete,
	[596] = trampoline_false, /* isDFBAllocated */
	[598] = trampoline_nop, /* find_swap_function_gated */
	[599] = dcpep_cb_main_display_v14_7, /* nested A410, expect true */
};

void DCP_FW_NAME(iomfb_start)(struct apple_dcp *dcp)
{
	dcp->cb_handlers = cb_handlers;

	dcp_start_signal(dcp, false, dcp_started, NULL);
}

/*
 * The session is usable once A455 has replied and the preferred timing has
 * been admitted, in either order.  The internal panel is the main display
 * and its connector is connected from then on.
 */
void iomfb_v14_7_try_activate(struct apple_dcp *dcp)
{
	struct dcp_v14_7_session *s = dcp->v14_7;

	if (dcp->active || !s->opened || !s->modes_ready || READ_ONCE(dcp->crashed))
		return;

	dcp->main_display = true;
	if (dcp->connector) {
		dcp->connector->connected = dcp->nr_modes > 0;
		schedule_work(&dcp->connector->hotplug_wq);
	}
	dcp->active = true;
	dev_info(dcp->dev, "IOMFB startup complete: %u callbacks, %u buffers; no modeset\n",
		 s->callbacks, s->objects);
	complete(&dcp->start_done);
}

/*
 * Latch a protocol failure of an adopted 14.7 session.  Nothing more is sent
 * or acknowledged; the firmware's buffers and the last scanout stay retained
 * until reboot, pending flips complete without success and a startup wait
 * returns.
 */
void iomfb_v14_7_fail(struct apple_dcp *dcp, const char *what)
{
	bool first = !READ_ONCE(dcp->crashed);

	WRITE_ONCE(dcp->crashed, true);
	if (first)
		dev_err(dcp->dev, "DCP session failed: %s; a reboot is required to restore the display\n",
			what);
	complete(&dcp->start_done);
	if (READ_ONCE(dcp->crtc))
		schedule_work(&dcp->vblank_wq);
}

static void iomfb_v14_7_session_free(void *data)
{
	dcp_property_store_destroy(data);
}

int iomfb_v14_7_session_init(struct apple_dcp *dcp)
{
	struct dcp_v14_7_session *s;
	int ret;

	s = devm_kzalloc(dcp->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->props = dcp_property_store_create();
	if (!s->props)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dcp->dev, iomfb_v14_7_session_free, s->props);
	if (ret)
		return ret;
	/* Failures can latch before dcp_start() initialises this. */
	init_completion(&dcp->start_done);
	dcp->v14_7 = s;
	return 0;
}

#undef DCP_FW_VER
#undef DCP_FW
