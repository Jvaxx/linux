// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * KUnit tests for the Type-C mux decisions of the Apple Type-C PHY driver.
 * This file is included from atc.c so that it can test static functions.
 */

#include <kunit/device.h>
#include <kunit/test.h>

static struct apple_atcphy *atcphy_test_alloc(struct kunit *test, bool fixed_usb2)
{
	struct apple_atcphy *atcphy;

	atcphy = kunit_kzalloc(test, sizeof(*atcphy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, atcphy);
	atcphy->fixed_usb2 = fixed_usb2;
	atcphy->mode = APPLE_ATCPHY_MODE_USB3_DP;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->host_active = fixed_usb2;

	return atcphy;
}

/* The mode lookup reports unsupported states against the PHY device */
static struct apple_atcphy *atcphy_test_alloc_dev(struct kunit *test, bool fixed_usb2)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, fixed_usb2);

	atcphy->dev = kunit_device_register(test, "atcphy-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, atcphy->dev);

	return atcphy;
}

static void atcphy_test_target_mode(struct kunit *test)
{
	struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
	struct typec_mux_state state = {};
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);

	state.mode = TYPEC_STATE_SAFE;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_OFF);

	state.mode = TYPEC_STATE_USB;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB3);

	state.alt = &dp;
	state.mode = TYPEC_DP_STATE_C;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_DP);
	state.mode = TYPEC_DP_STATE_D;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state),
			APPLE_ATCPHY_MODE_USB3_DP);
	state.mode = TYPEC_DP_STATE_E;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_DP);
	state.mode = TYPEC_DP_STATE_A;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_OFF);
}

/* A port behind a fixed hub keeps USB2 for its hub whenever a mode turns the PHY off */
static void atcphy_test_target_mode_fixed_hub(struct kunit *test)
{
	struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
	struct typec_mux_state state = {};
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, true);

	state.mode = TYPEC_STATE_SAFE;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB2);

	state.alt = &dp;
	state.mode = TYPEC_DP_STATE_A;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB2);
	state.mode = TYPEC_DP_STATE_D;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state),
			APPLE_ATCPHY_MODE_USB3_DP);
}

static void atcphy_test_noop_same_mode(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	/* A DisplayPort HPD or status update keeps the pin assignment */
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* The same holds while the controller does not use the USB3 PHY */
	atcphy->host_active = false;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* Ports without a fixed hub never route the PIPE from the mux */
	atcphy = atcphy_test_alloc(test, false);
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
}

static void atcphy_test_noop_needs_work(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	/* A new pin assignment or the safe state rebuild the lanes */
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_DP));
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB2));

	/* So does a failed transition or an orientation change */
	atcphy->mode_dirty = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
	atcphy->mode_dirty = false;

	/* A running controller is still owed its USB3 backend */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* or a handoff that has not completed */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
}

/*
 * A J700-style PHY behind a fixed hub, in DisplayPort + USB3 with the PIPE on
 * USB3 for a running host, but without a device tree node. A mux update that
 * goes for the USB3 root hub then fails with -ENODEV at the controller lookup,
 * before any register access, which tells the two paths apart.
 */
static struct apple_atcphy *atcphy_test_alloc_fixed_hub(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, true);

	atcphy->hw = &atcphy_hw_t8140;
	mutex_init(&atcphy->lock);

	return atcphy;
}

static const struct typec_altmode atcphy_test_dp = { .svid = USB_TYPEC_DP_SID };

static int atcphy_test_update_dp(struct apple_atcphy *atcphy, unsigned long pin)
{
	struct typec_mux_state state = {
		.alt = (struct typec_altmode *)&atcphy_test_dp,
		.mode = pin,
	};

	return atcphy_mux_update(atcphy, &state);
}

static void atcphy_test_update_skips_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	/* An HPD or status update with the same pin assignment */
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);
	KUNIT_EXPECT_FALSE(test, atcphy->mode_dirty);

	/* The orientation the lanes already use */
	KUNIT_EXPECT_EQ(test, atcphy_set_orientation(atcphy, TYPEC_ORIENTATION_NORMAL), 0);
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
}

static void atcphy_test_update_takes_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	/* A new pin assignment */
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENODEV);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);

	/* A PIPE handoff that has not completed */
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
	atcphy->pipe_pending = false;

	/* A running host whose PIPE is not on the USB3 backend */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;

	/* A cable flip moves the USB3 and DisplayPort lanes */
	KUNIT_EXPECT_EQ(test, atcphy_set_orientation(atcphy, TYPEC_ORIENTATION_REVERSE), 0);
	KUNIT_EXPECT_TRUE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
}

static struct kunit_case atcphy_mux_test_cases[] = {
	KUNIT_CASE(atcphy_test_target_mode),
	KUNIT_CASE(atcphy_test_target_mode_fixed_hub),
	KUNIT_CASE(atcphy_test_noop_same_mode),
	KUNIT_CASE(atcphy_test_noop_needs_work),
	KUNIT_CASE(atcphy_test_update_skips_usb),
	KUNIT_CASE(atcphy_test_update_takes_usb),
	{}
};

static struct kunit_suite atcphy_mux_test_suite = {
	.name = "phy-apple-atc-mux",
	.test_cases = atcphy_mux_test_cases,
};

kunit_test_suite(atcphy_mux_test_suite);
