// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "afk.h"

static void afk_ring_stride_test(struct kunit *test)
{
	u32 stride;

	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c0, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, stride, 0x40U);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x1180, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, stride, 0x80U);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x1000, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10bd, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c1, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0xc0, 0, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c3, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c0, 0x1001, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x11c0, 0x1040, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x1180, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, afk_ring_advance(0xf80, 0x80, 0x1000, stride), 0U);
	KUNIT_EXPECT_EQ(test, afk_ring_advance(0xe80, 0x80, 0x1000, stride),
			0xf00U);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c0, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, afk_ring_advance(0xfc0, 0x40, 0x1000, stride), 0U);
}

static void afk_ring_payload_fits_test(struct kunit *test)
{
	const size_t hdr = sizeof(struct afk_qe);

	/* An entry ending exactly at the end of the ring fits. */
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0, 0x1000 - hdr, 0x1000));
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0xfc0, 0x30, 0x1000));
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0x1000 - hdr, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0xfc0, 0x31, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0xff8, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x1000, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x1040, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0, 0, hdr - 1));
	/* Sizes that wrap a 32-bit offset sum must not pass for a fit. */
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x40, 0xfffffff8, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x40, U32_MAX - 0x3f, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x100, U32_MAX - 0xff, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0, U32_MAX, 0x1000));
}

static struct kunit_case afk_ring_cases[] = {
	KUNIT_CASE(afk_ring_stride_test),
	KUNIT_CASE(afk_ring_payload_fits_test),
	{}
};

static struct kunit_suite afk_ring_suite = {
	.name = "apple-afk-ring",
	.test_cases = afk_ring_cases,
};

kunit_test_suite(afk_ring_suite);

MODULE_LICENSE("Dual MIT/GPL");
