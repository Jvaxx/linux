// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple PMC voter interface: agent enables and the
 * AP's DCS vote, with memory standing in for the registers.
 *
 * Included by pmc.c.
 */

#include <kunit/test.h>

#define APPLE_PMC_TEST_RAILS_SIZE	0x4000

static struct apple_pmc *apple_pmc_test_alloc(struct kunit *test)
{
	const struct apple_pmc_hw *hw = &apple_pmc_hw_t8140;
	struct apple_pmc *pmc;

	pmc = kunit_kzalloc(test, sizeof(*pmc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pmc);
	pmc->hw = hw;
	raw_spin_lock_init(&pmc->lock);
	pmc->voters = (void __iomem *)kunit_kzalloc(test,
						    hw->agents * hw->agent_stride,
						    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)pmc->voters);
	pmc->rails = (void __iomem *)kunit_kzalloc(test,
						   APPLE_PMC_TEST_RAILS_SIZE,
						   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, (void __force *)pmc->rails);

	return pmc;
}

static void apple_pmc_test_set_dcs_vote(struct apple_pmc *pmc,
					unsigned int agent, unsigned int state)
{
	void __iomem *reg = pmc->voters + agent * pmc->hw->agent_stride;

	writel(state << APPLE_PMC_RAIL_SHIFT(pmc->hw->dcs_rail), reg);
}

/* Enabling and disabling an agent changes its bit on every rail only. */
static void apple_pmc_test_agent_enable(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	unsigned int rail;

	pmc->enable[0] = BIT(0);
	writel(BIT(0), pmc->rails + pmc->hw->rail_enable);

	KUNIT_ASSERT_EQ(test, apple_pmc_agent_enable(pmc, 1, true), 0);
	for (rail = 0; rail < pmc->hw->rails; rail++)
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail),
				BIT(1) | (rail ? 0 : BIT(0)));

	KUNIT_ASSERT_EQ(test, apple_pmc_agent_enable(pmc, 1, false), 0);
	for (rail = 0; rail < pmc->hw->rails; rail++)
		KUNIT_EXPECT_EQ(test, apple_pmc_enable_read(pmc, rail),
				rail ? 0 : BIT(0));
}

/* Lifting a DCS limit gives the AP's agent back its own vote, not 0. */
static void apple_pmc_test_dcs_limit_restores_ap_vote(struct kunit *test)
{
	struct apple_pmc *pmc = apple_pmc_test_alloc(test);
	const struct apple_pmc_hw *hw = pmc->hw;

	apple_pmc_test_set_dcs_vote(pmc, hw->ap_agent, 2);
	apple_pmc_test_set_dcs_vote(pmc, 0, 4);
	pmc->enable[hw->dcs_rail] = BIT(0) | BIT(hw->ap_agent);
	pmc->ap_dcs_vote = apple_pmc_ap_dcs_read(pmc);
	KUNIT_ASSERT_EQ(test, pmc->ap_dcs_vote, 2);

	/* A limit of 3 lets the AP vote at most DCS 1 for the PMP's 4. */
	pmc->dcs_state = 3;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 1);

	pmc->dcs_state = 0;
	apple_pmc_dcs_vote(pmc);
	KUNIT_EXPECT_EQ(test, apple_pmc_ap_dcs_read(pmc), 2);
}

static struct kunit_case apple_pmc_test_cases[] = {
	KUNIT_CASE(apple_pmc_test_agent_enable),
	KUNIT_CASE(apple_pmc_test_dcs_limit_restores_ap_vote),
	{}
};

static struct kunit_suite apple_pmc_test_suite = {
	.name = "apple-pmc",
	.test_cases = apple_pmc_test_cases,
};

kunit_test_suite(apple_pmc_test_suite);
