// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the H.264 slice checks of the Apple video decoder.
 */

#include <kunit/test.h>

#include "avd.h"

struct avd_slice_data_case {
	const char *desc;
	const u8 *data;
	u32 len;
	u32 header_len;
	bool found;
	u32 off;
};

static const u8 avd_test_slice[] = {
	0x25, 0xb8, 0x00, 0x00, 0x04, 0x01, 0xaa, 0xbb,
};

/* The same slice with an emulation prevention byte in its header. */
static const u8 avd_test_slice_epb[] = {
	0x25, 0xb8, 0x00, 0x00, 0x03, 0x01, 0xaa, 0xbb,
};

static const struct avd_slice_data_case avd_slice_data_cases[] = {
	{ "empty payload", avd_test_slice, 0, 0 },
	{ "one-byte payload", avd_test_slice, 1, 0 },
	{ "two-byte payload", avd_test_slice, 2, 0 },
	{ "one byte of data", avd_test_slice, 3, 2, true, 2 },
	{ "header ends one byte early", avd_test_slice, 8, 7, true, 7 },
	{ "header ends at the end", avd_test_slice, 8, 8 },
	{ "header beyond the end", avd_test_slice, 8, 12 },
	{ "header without emulation prevention", avd_test_slice, 8, 5, true, 5 },
	{ "emulation prevention in the header", avd_test_slice_epb, 8, 5, true, 6 },
};

KUNIT_ARRAY_PARAM_DESC(avd_slice_data, avd_slice_data_cases, desc);

static void avd_slice_data_test(struct kunit *test)
{
	const struct avd_slice_data_case *c = test->param_value;
	u32 off = U32_MAX;

	KUNIT_EXPECT_EQ(test, avd_h264_find_slice_data(c->data, c->len,
						       c->header_len, &off),
			c->found);
	if (c->found)
		KUNIT_EXPECT_EQ(test, off, c->off);
	else
		KUNIT_EXPECT_EQ(test, off, U32_MAX);
}

static struct kunit_case avd_h264_test_cases[] = {
	KUNIT_CASE_PARAM(avd_slice_data_test, avd_slice_data_gen_params),
	{}
};

static struct kunit_suite avd_h264_test_suite = {
	.name = "apple-avd-h264",
	.test_cases = avd_h264_test_cases,
};

kunit_test_suite(avd_h264_test_suite);
