// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/module.h>

#include "dcp-internal.h"

/*
 * An external H17P DCP back from system sleep on the same cable: suspend
 * stopped its pipe and advanced the cable generation, and the CRTC is off.
 */
struct external_resume_test {
	struct apple_dcp *dcp;
};

/* What the firmware hotplug callback does, under the same lock. */
static bool external_resume_hotplug(struct apple_dcp *dcp, bool connected)
{
	guard(mutex)(&dcp->iomfb.lock);
	return dcp_external_hotplug(dcp, connected);
}

static struct external_resume_test *external_resume_init(struct kunit *test)
{
	struct external_resume_test *priv;
	struct apple_dcp *dcp;
	struct device *dev;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dev = kunit_device_register(test, "dcp-external-resume");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dcp->dev = dev;
	dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
	spin_lock_init(&dcp->metadata.lock);
	mutex_init(&dcp->iomfb.lock);

	/* Connected and announced before the sleep. */
	dcp->typec_cable_connected = true;
	dcp->typec_generation = 1;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true);
	dcp_firmware_metadata_hotplug(dcp, true);
	dcp->external_cable = true;
	dcp->external_power_attempted = true;
	dcp->external_generation = 1;
	dcp->external_rx_generation = 1;
	dcp->external_crtc_generation = 1;

	/* Suspend: the pipe stopped, then the cable generation advanced. */
	dcp->external_stopped = true;
	dcp->iomfb.presentation_paused = true;
	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
	/* Resume: the reconnect work requested the link again. */
	dcp->dptxport[0].connected = true;

	priv->dcp = dcp;

	return priv;
}

/* The announcement after the link returns must count with the pipe stopped. */
static void external_paused_announce_recorded_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;

	KUNIT_ASSERT_FALSE(test, dcp->metadata.epoch.connected);
	/* A stopped pipe consumes the callback. */
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));

	KUNIT_EXPECT_TRUE(test, dcp->metadata.epoch.connected);
	KUNIT_EXPECT_EQ(test, dcp->metadata.epoch.generation, dcp->typec_generation);
	/* It is replayed to DRM only when the pipe starts again. */
	KUNIT_EXPECT_TRUE(test, dcp->external_hotplug_pending);
	KUNIT_EXPECT_TRUE(test, dcp->external_hotplug_connected);
}

/* The unplug a DPMS-off stop causes must not revoke the link proof. */
static void external_paused_unplug_ignored_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;
	u64 connection_sequence;

	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));
	connection_sequence = dcp->rate_proof.connection_sequence;
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, false));

	KUNIT_EXPECT_TRUE(test, dcp->metadata.epoch.connected);
	KUNIT_EXPECT_EQ(test, dcp->rate_proof.connection_sequence, connection_sequence);
}

/* An announcement on an older cable generation is not current. */
static void external_paused_stale_announce_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;

	dcp->typec_generation++;
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));

	KUNIT_EXPECT_FALSE(test, dcp->metadata.epoch.connected);
}

/*
 * A callback delayed past a release of the link is not an announcement of
 * the next one, even once software has moved both generations on.
 */
static void external_paused_released_link_test(struct kunit *test)
{
	struct external_resume_test *priv = external_resume_init(test);
	struct apple_dcp *dcp = priv->dcp;

	dcp->dptxport[0].connected = false;
	dcp->typec_generation++;
	dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false);
	KUNIT_EXPECT_TRUE(test, external_resume_hotplug(dcp, true));

	KUNIT_EXPECT_FALSE(test, dcp->metadata.epoch.connected);
}

static struct kunit_case external_resume_cases[] = {
	KUNIT_CASE(external_paused_announce_recorded_test),
	KUNIT_CASE(external_paused_unplug_ignored_test),
	KUNIT_CASE(external_paused_stale_announce_test),
	KUNIT_CASE(external_paused_released_link_test),
	{}
};

static struct kunit_suite external_resume_suite = {
	.name = "apple-dcp-external-resume",
	.test_cases = external_resume_cases,
};

kunit_test_suite(external_resume_suite);
