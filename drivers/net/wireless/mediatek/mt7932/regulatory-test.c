// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the regulatory snapshot logic and, as local files the
 * driver must not trust, the country power packages.
 */
#include <kunit/test.h>

#include "mt7932.h"

/* A world-domain snapshot: channels 1, 6, 36 and 149, the 5 GHz ones NO_IR. */
static void reg_build(struct mt7932_reg_snapshot *reg)
{
	static const u16 channels[] = {1, 6, 36, 149};
	unsigned int i;

	memset(reg, 0, sizeof(*reg));
	memcpy(reg->alpha2, "00", 2);
	memcpy(reg->domain, "XZ", 2);
	reg->domain[4] = 1;
	reg->domain[5] = 2;
	reg->domain[8] = 2;
	reg->domain[9] = 2;
	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		put_unaligned_le16(channels[i], reg->domain + 12 + i * 8);
		put_unaligned_le32(channels[i] > 14 ? IEEE80211_CHAN_NO_IR : 0,
				   reg->domain + 16 + i * 8);
		reg->power[i] = 20;
	}
	reg->length = 12 + ARRAY_SIZE(channels) * 8;
}

static void reg_set_flags(struct mt7932_reg_snapshot *reg, unsigned int entry, u32 flags)
{
	put_unaligned_le32(flags, reg->domain + 16 + entry * 8);
}

/* A beacon hint lifting NO_IR only relaxes; nothing else does. */
static void mt7932_reg_relaxation_test(struct kunit *test)
{
	struct mt7932_reg_snapshot *prev, *next;

	prev = kunit_kzalloc(test, sizeof(*prev), GFP_KERNEL);
	next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, prev);
	KUNIT_ASSERT_NOT_NULL(test, next);
	reg_build(prev);

	reg_build(next);
	reg_set_flags(next, 2, 0);
	KUNIT_EXPECT_TRUE(test, mt7932_reg_only_relaxes(prev, next));
	/* An identical snapshot is not treated as a change by the caller,
	 * but it is trivially within the applied domain.
	 */
	KUNIT_EXPECT_TRUE(test, mt7932_reg_only_relaxes(prev, prev));

	/* NO_IR added: a restriction. */
	reg_build(next);
	reg_set_flags(next, 0, IEEE80211_CHAN_NO_IR);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	/* Radar or a bandwidth limit appears, or another flag goes away. */
	reg_build(next);
	reg_set_flags(next, 2, IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_set_flags(prev, 3, IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40);
	reg_build(next);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(prev);
	/* A new country, power limit, channel or channel order. */
	reg_build(next);
	memcpy(next->alpha2, "AU", 2);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	next->power[1] = 19;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	put_unaligned_le16(40, next->domain + 12 + 3 * 8);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	put_unaligned_le16(44, next->domain + 12 + 4 * 8);
	next->domain[9] = 3;
	next->length += 8;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	/* A failed snapshot never relaxes, and a corrupt length is refused. */
	reg_build(next);
	next->error = -EOPNOTSUPP;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	prev->length = sizeof(prev->domain) + 8;
	next->length = prev->length;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
}

static struct kunit_case mt7932_policy_test_cases[] = {
	KUNIT_CASE(mt7932_reg_relaxation_test),
	{}
};

static struct kunit_suite mt7932_policy_test_suite = {
	.name = "mt7932-regulatory",
	.test_cases = mt7932_policy_test_cases,
};

kunit_test_suite(mt7932_policy_test_suite);
