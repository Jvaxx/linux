// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the USB core's wakeup decisions in system sleep
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>

/* hub.h includes usb.h, which has no include guard */
#include "hub.h"

#define USB_WAKEUP_TEST_PORTS	4U

/* A suspended USB device, with the hub driver state of a hub */
struct usb_wakeup_test_dev {
	struct usb_device udev;
	struct usb_host_config config;
	struct usb_interface intf;
	struct usb_hub hub;
	struct usb_port ports[USB_WAKEUP_TEST_PORTS];
	struct usb_port *port_list[USB_WAKEUP_TEST_PORTS];
};

/* A hub or another device, attached to the first free port of @parent if any */
static struct usb_wakeup_test_dev *usb_wakeup_test_add(struct kunit *test,
						       struct usb_wakeup_test_dev *parent,
						       bool hub)
{
	struct usb_wakeup_test_dev *d = kunit_kzalloc(test, sizeof(*d), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, d);
	if (hub) {
		for (i = 0; i < USB_WAKEUP_TEST_PORTS; i++)
			d->port_list[i] = &d->ports[i];
		d->hub.hdev = &d->udev;
		d->hub.ports = d->port_list;
		d->config.interface[0] = &d->intf;
		usb_set_intfdata(&d->intf, &d->hub);
		d->udev.actconfig = &d->config;
		d->udev.maxchild = USB_WAKEUP_TEST_PORTS;
	}
	if (parent) {
		for (i = 0; i < USB_WAKEUP_TEST_PORTS && parent->ports[i].child; i++)
			;
		KUNIT_ASSERT_LT(test, i, USB_WAKEUP_TEST_PORTS);
		parent->ports[i].child = &d->udev;
		d->udev.parent = &parent->udev;
	}

	return d;
}

/*
 * The defaults behind the front port of an Apple J700: a keyboard with remote
 * wakeup enabled, below a hub, below the VIA Labs VL122 hub on the root port,
 * every hub with wakeup disabled. The VL122 does not pass a remote wakeup on
 * while it is suspended, so the keyboard's does not reach the root hub, unless
 * the VL122 is kept awake.
 */
static void usb_wakeup_reaches_vl122_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *vl122 = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, vl122, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	vl122->udev.quirks = USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP;
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&hub->udev));
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&vl122->udev));
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));

	vl122->udev.awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/* Any other hub passes it on, whatever its own wakeup setting. */
static void usb_wakeup_reaches_hub_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	usb_wakeup_test_add(test, hub, false);
	usb_wakeup_test_add(test, root, false);
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));

	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/*
 * A hub armed for wakeup signals connect, disconnect and over-current changes
 * on its ports, also if it does not pass wakeups on from below while suspended.
 */
static void usb_wakeup_reaches_armed_hub_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);

	hub->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));

	hub->udev.quirks = USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));
}

/*
 * A hub that does not pass wakeups on from below while it is suspended, armed
 * for wakeup, with wakeup disabled for the root hub and the device below it:
 * its own changes reach the root hub. A remote wakeup from below it does not.
 */
static void usb_wakeup_reaches_armed_quirky_hub_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *hub = usb_wakeup_test_add(test, root, true);
	struct usb_wakeup_test_dev *keyboard = usb_wakeup_test_add(test, hub, false);

	hub->udev.quirks = USB_QUIRK_HUB_NO_SUSPENDED_WAKEUP;
	hub->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, usb_wakeup_reaches(&root->udev));

	hub->udev.do_remote_wakeup = 0;
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));
}

/* A root hub with nothing below, and a device that is no hub, reach nothing. */
static void usb_wakeup_reaches_nothing_test(struct kunit *test)
{
	struct usb_wakeup_test_dev *root = usb_wakeup_test_add(test, NULL, true);
	struct usb_wakeup_test_dev *keyboard;

	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&root->udev));
	keyboard = usb_wakeup_test_add(test, root, false);
	keyboard->udev.do_remote_wakeup = 1;
	KUNIT_EXPECT_FALSE(test, usb_wakeup_reaches(&keyboard->udev));
}

static struct kunit_case usb_wakeup_test_cases[] = {
	KUNIT_CASE(usb_wakeup_reaches_vl122_test),
	KUNIT_CASE(usb_wakeup_reaches_hub_test),
	KUNIT_CASE(usb_wakeup_reaches_armed_hub_test),
	KUNIT_CASE(usb_wakeup_reaches_armed_quirky_hub_test),
	KUNIT_CASE(usb_wakeup_reaches_nothing_test),
	{}
};

static struct kunit_suite usb_wakeup_test_suite = {
	.name = "usb_wakeup",
	.test_cases = usb_wakeup_test_cases,
};

kunit_test_suite(usb_wakeup_test_suite);

MODULE_DESCRIPTION("KUnit tests for the USB core's wakeup decisions in system sleep");
MODULE_LICENSE("GPL");
