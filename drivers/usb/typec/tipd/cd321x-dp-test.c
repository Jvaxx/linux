// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "cd321x-dp.h"

/* A dock in pin assignment D: DisplayPort plus USB 3. */
#define DP_D	(TPS_DATA_STATUS_DATA_CONNECTION | TPS_DATA_STATUS_USB2_CONNECTION | \
		 TPS_DATA_STATUS_USB3_CONNECTION | TPS_DATA_STATUS_DP_CONNECTION | \
		 FIELD_PREP(TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK, 1))
#define HPD	CD321X_DATA_STATUS_HPD_LEVEL

static void cd321x_dp_first_hpd_connects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = DP_D | HPD,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* Entering DP without HPD sets up the route but announces nothing. */
static void cd321x_dp_route_without_hpd_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D,
		.changed = DP_D,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_NONE);
}

static void cd321x_dp_unplug_tears_down_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.changed = DP_D | HPD,
		.link_reset = true,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, false), CD321X_DP_EVENT_NONE);
}

/* A flip or a new partner rebuilds the route even with HPD still high. */
static void cd321x_dp_link_reset_reconnects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.link_reset = true,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

static void cd321x_dp_hpd_low_tears_down_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D,
		.changed = HPD,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_NONE);
}

static void cd321x_dp_exit_tears_down_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = TPS_DATA_STATUS_DATA_CONNECTION |
			       TPS_DATA_STATUS_USB2_CONNECTION |
			       TPS_DATA_STATUS_USB3_CONNECTION,
		.changed = TPS_DATA_STATUS_DP_CONNECTION | HPD |
			   TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, false), CD321X_DP_EVENT_NONE);
}

/* A new pin assignment rebuilds the lanes under the stream. */
static void cd321x_dp_pin_change_reconnects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A power or USB-only update leaves a live display alone. */
static void cd321x_dp_unrelated_update_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = TPS_DATA_STATUS_USB_DATA_ROLE,
		.route_active = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A port that does not own the display never reports for it. */
static void cd321x_dp_display_taken_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = DP_D | HPD,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, false), CD321X_DP_EVENT_NONE);
	u.data_status = 0;
	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
}

static struct kunit_case cd321x_dp_cases[] = {
	KUNIT_CASE(cd321x_dp_first_hpd_connects_test),
	KUNIT_CASE(cd321x_dp_route_without_hpd_test),
	KUNIT_CASE(cd321x_dp_unplug_tears_down_test),
	KUNIT_CASE(cd321x_dp_link_reset_reconnects_test),
	KUNIT_CASE(cd321x_dp_hpd_low_tears_down_test),
	KUNIT_CASE(cd321x_dp_exit_tears_down_test),
	KUNIT_CASE(cd321x_dp_pin_change_reconnects_test),
	KUNIT_CASE(cd321x_dp_unrelated_update_test),
	KUNIT_CASE(cd321x_dp_display_taken_test),
	{}
};

static struct kunit_suite cd321x_dp_suite = {
	.name = "cd321x-dp",
	.test_cases = cd321x_dp_cases,
};

kunit_test_suite(cd321x_dp_suite);
MODULE_DESCRIPTION("KUnit tests for the CD321x DisplayPort hotplug decisions");
MODULE_LICENSE("GPL");
