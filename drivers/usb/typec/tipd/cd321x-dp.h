/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CD321X_DP_H__
#define __CD321X_DP_H__

#include <linux/types.h>

#include "tps6598x.h"

/* DATA_STATUS bits whose change rebuilds the DisplayPort lanes. */
#define CD321X_DP_MODE_BITS \
	(TPS_DATA_STATUS_DP_CONNECTION | TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK)

/* What the display connector is told once a cable update has been applied. */
enum cd321x_dp_event {
	CD321X_DP_EVENT_NONE,
	CD321X_DP_EVENT_CONNECTED,
	CD321X_DP_EVENT_RETRAIN,
};

/**
 * struct cd321x_dp_update - DisplayPort view of one cable update
 * @data_status: DATA_STATUS of this update
 * @changed: DATA_STATUS bits that changed since the previous update
 * @link_reset: the Type-C session is torn down: unplug, flip, partner
 *	change, Thunderbolt/USB4 entry or exit, or a forced reconnect
 * @route_active: the previous update left a display route up
 * @sink_reported: the connector was last told that a sink is present
 */
struct cd321x_dp_update {
	u32 data_status;
	u32 changed;
	bool link_reset;
	bool route_active;
	bool sink_reported;
};

/*
 * USB3_CONNECTION is part of the pin assignment while DisplayPort is up: C
 * and D, and E and F, differ only in it, and moving between them changes
 * the DisplayPort lane count.  Without DisplayPort it is a plain USB change.
 */
static inline bool cd321x_dp_mode_changed(u32 data_status, u32 changed)
{
	u32 bits = CD321X_DP_MODE_BITS;

	if (data_status & TPS_DATA_STATUS_DP_CONNECTION)
		bits |= TPS_DATA_STATUS_USB3_CONNECTION;

	return changed & bits;
}

/*
 * HPD dropped for less than the debounce delay and came back high while the
 * partner, the alternate mode and the pin assignment stayed the same.  The
 * sink is still there, so check and retrain its link rather than report an
 * unplug.
 */
static inline bool cd321x_dp_hpd_pulse(const struct cd321x_dp_update *u)
{
	return u->route_active && !u->link_reset && u->sink_reported &&
	       (u->data_status & TPS_DATA_STATUS_DP_CONNECTION) &&
	       (u->data_status & CD321X_DATA_STATUS_HPD_LEVEL) &&
	       (u->changed & CD321X_DATA_STATUS_HPD_LEVEL) &&
	       !cd321x_dp_mode_changed(u->data_status, u->changed);
}

/* Does the display connector lose its sink before this update is applied? */
static inline bool cd321x_dp_teardown(const struct cd321x_dp_update *u)
{
	if (!u->route_active)
		return false;

	if (u->link_reset ||
	    !(u->data_status & TPS_DATA_STATUS_DP_CONNECTION) ||
	    !(u->data_status & CD321X_DATA_STATUS_HPD_LEVEL) ||
	    cd321x_dp_mode_changed(u->data_status, u->changed))
		return true;

	return (u->changed & CD321X_DATA_STATUS_HPD_LEVEL) &&
	       !cd321x_dp_hpd_pulse(u);
}

/* @route_up: the update left this port's display route up. */
static inline enum cd321x_dp_event
cd321x_dp_event(const struct cd321x_dp_update *u, bool route_up)
{
	if (!route_up || !(u->data_status & CD321X_DATA_STATUS_HPD_LEVEL))
		return CD321X_DP_EVENT_NONE;

	/* The sink stays connected: have its link checked, not re-announced. */
	if (cd321x_dp_hpd_pulse(u))
		return CD321X_DP_EVENT_RETRAIN;
	if (u->route_active && u->sink_reported && !cd321x_dp_teardown(u) &&
	    (u->data_status & u->changed & CD321X_DATA_STATUS_HPD_IRQ))
		return CD321X_DP_EVENT_RETRAIN;

	return CD321X_DP_EVENT_CONNECTED;
}

#endif /* __CD321X_DP_H__ */
