/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * MacBook Pro (14-inch, M3 Pro) J514S external display controllers.
 *
 * DCPEXT0 and DCPEXT1 run the same DCP firmware 14.7.0 image as the internal
 * panel's DCP, in its external role. The boot loader reserves their firmware
 * and publishes a controller only then; Linux starts it cold. DCPEXT1 drives
 * the three USB-C ports through the DisplayPort alternate mode: the Type-C
 * port controller reports the pin assignment and HPD to the controller's
 * Type-C routes, the ATC PHY of the port carries AUX and the main link, and
 * the port's display crossbar connects the controller's output to that PHY.
 *
 * The helpers below hold no driver state, so that KUnit can check the DPTX
 * callback replies, the HPD/IRQ_HPD bookkeeping and the catalog's link
 * budget without hardware.
 */

#ifndef __APPLE_DCPEXT_T6030_H__
#define __APPLE_DCPEXT_T6030_H__

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

/* Coprocessor windows of the two controllers (ADT /arm-io/dcpext0, dcpext1). */
#define J514S_DCPEXT0_COPROC	0x2d2c00000ULL
#define J514S_DCPEXT1_COPROC	0x2d6c00000ULL

/*
 * DPTX link profile of the USB-C route: RBR, HBR and HBR2 only, at most two
 * lanes on pin D and four on pin C, drive levels 0..3 with voltage +
 * pre-emphasis <= 3.
 */
#define DCPEXT_RATE_RBR		0x06
#define DCPEXT_RATE_HBR		0x0a
#define DCPEXT_RATE_HBR2	0x14
#define DCPEXT_RATE_MAX		DCPEXT_RATE_HBR2
#define DCPEXT_MAX_LANES	4
#define DCPEXT_DRIVE_MAX	3
/* Payload of one lane at the highest admitted rate: 5.4 Gb/s, 8b/10b. */
#define DCPEXT_LANE_KBPS	4320000U
/* Uncompressed wire bits per pixel the catalog is budgeted with (RGB8). */
#define DCPEXT_WIRE_BPP		24
/* Timings the external pipe is qualified with. */
#define DCPEXT_MAX_WIDTH	3840
#define DCPEXT_MAX_HEIGHT	2160
#define DCPEXT_MIN_RATE_16	(24U << 16)
#define DCPEXT_MAX_RATE_16	(120U << 16)

/* Callback data offsets (relative to the callback's data). */
#define DCPEXT_CB_VALUE		16	/* u32 or u64 argument / result */
#define DCPEXT_CB_VALUE2	20	/* second u32 (max pre-emphasis, SSC) */
#define DCPEXT_CB_DRIVE		32	/* per-lane voltage +0, pre-emphasis +4 */

/* The routed port's DPTX link, as the external profile reports it. */
struct dcpext_link {
	u32 lanes_max;		/* the pin assignment's width: 2 (D), 4 (C) */
	u32 lanes;		/* active lanes (callback 12) */
	u32 rate;		/* link-rate code (callback 9) */
	u32 drive[DCPEXT_MAX_LANES][2];	/* programmed voltage, pre-emphasis */
	bool aux;		/* AUX on (callback 0) */
	bool xbar_up;		/* crossbar connection up */
	bool linked;		/* HPD high delivered, not released */
	bool inactive;		/* callback 20 without a link-up since */
};

/* The hardware step a DPTX callback needs before it can be acknowledged. */
enum dcpext_link_hw {
	DCPEXT_HW_NONE,
	DCPEXT_HW_AUX_ON,	/* phy_set_mode(DP): AUX on */
	DCPEXT_HW_DEACTIVATE,	/* crossbar down, phy_set_mode(INVALID): rate 0, AUX off */
	DCPEXT_HW_XBAR_DOWN,
	DCPEXT_HW_XBAR_UP,
	DCPEXT_HW_RATE,		/* crossbar down, phy_configure(set_rate) */
	DCPEXT_HW_LANES,	/* phy_configure(set_lanes) */
	DCPEXT_HW_DRIVE,	/* phy_configure(set_voltages) */
};

struct dcpext_link_op {
	enum dcpext_link_hw hw;
	u32 value;		/* rate code, lane count */
	u32 lanes;		/* drive request: its lane count */
	u32 voltage[DCPEXT_MAX_LANES];	/* requested, then as programmed */
	u32 pre[DCPEXT_MAX_LANES];
};

int dcpext_link_plan(const struct dcpext_link *link, u32 index,
		     const void *data, size_t size, struct dcpext_link_op *op);
int dcpext_link_reply(struct dcpext_link *link, u32 index,
		      const struct dcpext_link_op *op, const void *data,
		      size_t size, void *reply, size_t room);

/*
 * HPD and IRQ_HPD of the routed sink.  Every HPD transition starts a new
 * generation and drops queued IRQs; an IRQ_HPD counts only while the sink's
 * HPD is high and has been delivered to the firmware.
 */
struct dcpext_hpd {
	u64 generation;
	bool high;
	unsigned int irq_pending;
	u64 irq_generation;
	u32 irq_queued, irq_dropped;
};

bool dcpext_hpd_set(struct dcpext_hpd *hpd, bool high);
bool dcpext_hpd_irq(struct dcpext_hpd *hpd, bool delivered);
bool dcpext_hpd_irq_take(struct dcpext_hpd *hpd, u64 *generation);

int dcpext_edid_object(const u8 *object, size_t len, const u8 **edid);

struct dcp_display_mode;
unsigned int dcpext_catalog_filter(struct dcp_display_mode *modes, unsigned int count,
				   u32 lanes, unsigned int *rejected);

/* DPTX port commands of the external role. */
enum dcpext_dptx_cmd {
	DCPEXT_DPTX_CONNECT,
	DCPEXT_DPTX_REQUEST,
	DCPEXT_DPTX_HPD_LOW,
	DCPEXT_DPTX_HPD_HIGH,
	DCPEXT_DPTX_RELEASE,
	DCPEXT_DPTX_IRQ,
};

/* What the firmware holds of the current sink connection. */
enum dcpext_dptx_state {
	DCPEXT_DPTX_NONE,
	DCPEXT_DPTX_CONNECTED,	/* CONNECT replied */
	DCPEXT_DPTX_REQUESTED,	/* REQUEST_DISPLAY replied */
	DCPEXT_DPTX_HPD,	/* HPD high replied */
};

struct apple_dcp;
struct apple_dcp_typec_route;

/* The external role's state, hung off the 14.7 session. */
struct dcp_ext_v14_7 {
	struct apple_dcp *dcp;

	/* Under dcp->external_lock. */
	bool drm_ready;		/* the DRM device bound: a start is allowed */
	bool started;		/* RUN issued: the session is never stopped */
	bool display_on;	/* A472 on */
	enum dcpext_dptx_state dptx;
	int link_error;		/* last failed lifecycle RPC of this generation */
	u64 link_error_generation;
	u32 timing_id, color_id;	/* the last A411 */
	u32 connects, releases;

	/* D101: 7680 until a mode is set, then the mode's pitch. */
	u32 stride;
	/* HPD high delivered and not released; read by the RX and IRQ paths. */
	bool hpd_delivered;

	/* HPD generation and IRQ_HPD queue. */
	spinlock_t lock;
	struct dcpext_hpd hpd;

	/* The held route and its link, under link_lock (DPTX callbacks). */
	struct mutex link_lock;
	struct apple_dcp_typec_route *route;
	bool xbar_selected;	/* the route holds its crossbar selection */
	struct dcpext_link link;

	/* The sink's catalog: published for one HPD generation. */
	bool catalog_valid;
	u64 catalog_generation;
	unsigned int catalog_admitted, catalog_rejected;
	u32 catalog_lanes;

	/* AV service (EP 0x28): opened once per publication, under dcpavserv.lock. */
	bool av_opened;
	u64 av_epoch;
	int av_result;

	/* Startup, connection and failure waits. */
	wait_queue_head_t wq;
	struct work_struct connect_work;
	struct work_struct irq_work;
	struct work_struct av_work;

	/* Reported by the dcpext debugfs file. */
	u32 callbacks[32];
	u32 callbacks_refused;
	u32 sink_errors;
	u32 irq_sent, irq_ok, irq_failed;
	u32 presents_refused;
	u32 edid_reads, edid_errors;
};

#endif /* __APPLE_DCPEXT_T6030_H__ */
