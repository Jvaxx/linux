/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Aurora Silicon */
#ifndef APPLE_DCP_EXTERNAL_BW_H
#define APPLE_DCP_EXTERNAL_BW_H

#include <linux/types.h>

static inline bool apple_dcp_external_bw_valid(u64 size, int scratch, int request)
{
	if (size < sizeof(u32) || scratch < 0 || request < 0)
		return false;
	if ((scratch & 3) || (request & 3) || scratch == request)
		return false;
	return (u64)scratch <= size - sizeof(u32) &&
	       (u64)request <= size - sizeof(u32);
}

static inline bool apple_dcp_external_clock_window(u64 start, u64 size,
						   u64 expected_start, u64 expected_size)
{
	return start == expected_start && size == expected_size;
}

#endif
