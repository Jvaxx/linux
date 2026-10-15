// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/bitfield.h>
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

/*
 * HPD went low and back within the debounce on the same dock and lanes: the
 * route is kept and its link rechecked, so the picture does not drop.
 */
static void cd321x_dp_hpd_pulse_retrains_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_RETRAIN);
}

/* An IRQ_HPD attention with HPD held high asks for a link check. */
static void cd321x_dp_irq_hpd_retrains_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.changed = CD321X_DATA_STATUS_HPD_IRQ,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_RETRAIN);

	/* The attention bit clearing again is not a new request. */
	u.data_status &= ~CD321X_DATA_STATUS_HPD_IRQ;
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* HPD rising after a reported low is a connect, as before. */
static void cd321x_dp_hpd_rise_connects_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD,
		.route_active = true,
	};

	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
	/* Attention is no reason to skip announcing a sink that just came up. */
	u.data_status |= CD321X_DATA_STATUS_HPD_IRQ;
	u.changed |= CD321X_DATA_STATUS_HPD_IRQ;
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* C <-> D changes the DisplayPort lane count: no pulse, a reconnect. */
static void cd321x_dp_pulse_with_lane_change_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD | TPS_DATA_STATUS_USB3_CONNECTION,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);

	u.changed = HPD | TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK;
	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A replug, flip or new dock inside the debounce is still a reconnect. */
static void cd321x_dp_pulse_with_link_reset_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.changed = HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.link_reset = true,
		.route_active = true,
		.sink_reported = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/* A route without a delivered HPD has nothing to retrain yet. */
static void cd321x_dp_retrain_needs_live_sink_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD | CD321X_DATA_STATUS_HPD_IRQ,
		.changed = CD321X_DATA_STATUS_HPD_IRQ,
		.route_active = true,
	};

	KUNIT_EXPECT_FALSE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);

	/* A port that just entered DP connects rather than retrains. */
	u.route_active = false;
	u.sink_reported = true;
	u.changed = HPD;
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
}

/*
 * A pass that reported the sink gone and then failed leaves the route
 * marked up.  When the retry finds HPD high again the sink must be
 * announced, not retrained: the connector has no link left to retrain.
 */
static void cd321x_dp_retry_after_teardown_test(struct kunit *test)
{
	struct cd321x_dp_update u = {
		.data_status = DP_D | HPD,
		.changed = HPD,
		.route_active = true,
	};

	KUNIT_EXPECT_TRUE(test, cd321x_dp_teardown(&u));
	KUNIT_EXPECT_EQ(test, cd321x_dp_event(&u, true), CD321X_DP_EVENT_CONNECTED);
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
	KUNIT_CASE(cd321x_dp_hpd_pulse_retrains_test),
	KUNIT_CASE(cd321x_dp_irq_hpd_retrains_test),
	KUNIT_CASE(cd321x_dp_hpd_rise_connects_test),
	KUNIT_CASE(cd321x_dp_pulse_with_lane_change_test),
	KUNIT_CASE(cd321x_dp_pulse_with_link_reset_test),
	KUNIT_CASE(cd321x_dp_retrain_needs_live_sink_test),
	KUNIT_CASE(cd321x_dp_retry_after_teardown_test),
	{}
};

static struct kunit_suite cd321x_dp_suite = {
	.name = "cd321x-dp",
	.test_cases = cd321x_dp_cases,
};

kunit_test_suite(cd321x_dp_suite);
MODULE_DESCRIPTION("KUnit tests for the CD321x DisplayPort hotplug decisions");
MODULE_LICENSE("GPL");
