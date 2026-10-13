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

/* Resolve optional diagnostic overrides without changing the SoC defaults. */
static inline bool apple_dcp_external_bw_resolve(u64 size, int default_scratch,
						int default_request, int override_scratch,
						int override_request, u32 *scratch, u32 *request)
{
	int s = override_scratch == -1 ? default_scratch : override_scratch;
	int r = override_request == -1 ? default_request : override_request;

	if (!apple_dcp_external_bw_valid(size, s, r))
		return false;
	*scratch = s;
	*request = r;
	return true;
}

#endif
