// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#ifndef __APPLE_VERSION_UTILS_H__
#define __APPLE_VERSION_UTILS_H__

#include <linux/kernel.h>
#include <linux/args.h>

#define DCP_FW_UNION(u) (u).DCP_FW
#define DCP_FW_SUFFIX CONCATENATE(_, DCP_FW)
#define DCP_FW_NAME(name) CONCATENATE(name, DCP_FW_SUFFIX)
#define DCP_FW_VERSION(x, y, z) ( ((x) << 16) | ((y) << 8) | (z) )

/*
 * The swap record layout introduced with the 14.7 firmware.  The
 * H17-generation firmware (26.0 and later) keeps the earlier layout and
 * extends it with its own fields.
 */
#define DCP_FW_LAYOUT_14_7(ver) \
	((ver) >= DCP_FW_VERSION(14, 7, 0) && (ver) < DCP_FW_VERSION(26, 0, 0))

enum dcp_firmware_version {
	DCP_FIRMWARE_UNKNOWN,
	DCP_FIRMWARE_V_12_3,
	DCP_FIRMWARE_V_13_5,
	DCP_FIRMWARE_V_14_7,
	DCP_FIRMWARE_H17P,
};

#endif /*__APPLE_VERSION_UTILS_H__*/
