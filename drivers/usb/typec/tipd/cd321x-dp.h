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
};

/**
 * struct cd321x_dp_update - DisplayPort view of one cable update
 * @data_status: DATA_STATUS of this update
 * @changed: DATA_STATUS bits that changed since the previous update
 * @link_reset: the Type-C session is torn down: unplug, flip, partner
 *	change, Thunderbolt/USB4 entry or exit, or a forced reconnect
 * @route_active: the previous update left a display route up
 */
struct cd321x_dp_update {
	u32 data_status;
	u32 changed;
	bool link_reset;
	bool route_active;
};

static inline bool cd321x_dp_mode_changed(u32 changed)
{
	return changed & CD321X_DP_MODE_BITS;
}

/* Does the display connector lose its sink before this update is applied? */
static inline bool cd321x_dp_teardown(const struct cd321x_dp_update *u)
{
	if (!u->route_active)
		return false;

	return u->link_reset ||
	       !(u->data_status & TPS_DATA_STATUS_DP_CONNECTION) ||
	       !(u->data_status & CD321X_DATA_STATUS_HPD_LEVEL) ||
	       (u->changed & CD321X_DATA_STATUS_HPD_LEVEL) ||
	       cd321x_dp_mode_changed(u->changed);
}

/* @route_up: the update left this port's display route up. */
static inline enum cd321x_dp_event
cd321x_dp_event(const struct cd321x_dp_update *u, bool route_up)
{
	if (!route_up || !(u->data_status & CD321X_DATA_STATUS_HPD_LEVEL))
		return CD321X_DP_EVENT_NONE;

	return CD321X_DP_EVENT_CONNECTED;
}

#endif /* __CD321X_DP_H__ */
