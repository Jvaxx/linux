// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/soc/apple/pmp-power-policy.h>
#include "apple-pmp-confirm.h"

static void apple_pmp_test_fault_recovery(struct kunit *test)
{
	struct apple_pmp_cooling state;
	unsigned int i;

	apple_pmp_cooling_init(&state);
	for (i = 0; i < 100; i++)
		apple_pmp_cooling_sample(&state, 20000000, 50000, 0);
	KUNIT_EXPECT_TRUE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.good_reads, 1U);
	for (i = 2; i <= 10; i++)
		apple_pmp_cooling_sample(&state, i * 20000000ULL, 50000, 0);
	KUNIT_EXPECT_FALSE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.level, 8U);
	apple_pmp_cooling_sample(&state, 400000000, 50000, 0);
	KUNIT_EXPECT_EQ(test, state.level, 7U);
	apple_pmp_cooling_sample(&state, 405000000, 50000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.level, 8U);
}

static void apple_pmp_test_critical_confirmation(struct kunit *test)
{
	struct apple_pmp_confirmation state = {};
	unsigned int i;

	for (i = 0; i < 100; i++)
		apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, 0, 10000000);
	KUNIT_EXPECT_EQ(test, state.count, 1U);
	KUNIT_EXPECT_FALSE(test, state.confirmed);
	apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, -EIO, 15000000);
	KUNIT_EXPECT_EQ(test, state.count, 0U);
	for (i = 0; i < 3; i++)
		apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, 0,
					      20000000 + i * 5000000ULL);
	KUNIT_EXPECT_TRUE(test, state.confirmed);
	apple_pmp_confirmation_sample(&state, 0, -EIO, 40000000);
	KUNIT_EXPECT_TRUE(test, state.confirmed);
}

static void apple_pmp_test_power_validation(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 mw;
	u32 rails[3] = { 4000, 1000, 0 };

	KUNIT_EXPECT_TRUE(test, apple_pmp_power_decode(0x3f800000, &mw));
	KUNIT_EXPECT_EQ(test, mw, 1000U);
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0x7f800000, &mw));
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0xbf800000, &mw));
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0x42500000, &mw));
	apple_pmp_power_init(&state);
	apple_pmp_power_sample(&state, 500000000, rails, 60000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.fault);
	KUNIT_EXPECT_EQ(test, state.level[0], 8U);
	KUNIT_EXPECT_EQ(test, state.level[1], 8U);
}

static struct kunit_case apple_pmp_policy_cases[] = {
	KUNIT_CASE(apple_pmp_test_fault_recovery),
	KUNIT_CASE(apple_pmp_test_critical_confirmation),
	KUNIT_CASE(apple_pmp_test_power_validation),
	{},
};

static struct kunit_suite apple_pmp_policy_suite = {
	.name = "apple-pmp-policy",
	.test_cases = apple_pmp_policy_cases,
};
kunit_test_suite(apple_pmp_policy_suite);
MODULE_LICENSE("Dual MIT/GPL");
