/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

/* ── helpers ─────────────────────────────────────────────────────────── */

/* ── tests ───────────────────────────────────────────────────────────── */

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_watchdog_create_destroy(void **state)
{
	(void)state;
	ove_watchdog_t wd = NULL;
	int rc = ove_watchdog_create(&wd, 5000);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(wd);
	ove_watchdog_destroy(wd);
}

/* On silicon the STM32 IWDG cannot be stopped once started, so a dog these
 * tests arm would reset the board a few suites later and restart the run.
 * test_hw_stm32f746.c checks the real watchdog (start and reset) instead. */
#ifndef OVE_HW
static void test_watchdog_start(void **state)
{
	(void)state;
	ove_watchdog_t wd = NULL;
	OVE_TEST_ASSERT_OK(ove_watchdog_create(&wd, 5000));
	int rc = ove_watchdog_start(wd);
	assert_int_equal(rc, OVE_OK);
	ove_watchdog_stop(wd);
	ove_watchdog_destroy(wd);
}

static void test_watchdog_feed(void **state)
{
	(void)state;
	ove_watchdog_t wd = NULL;
	OVE_TEST_ASSERT_OK(ove_watchdog_create(&wd, 5000));
	OVE_TEST_ASSERT_OK(ove_watchdog_start(wd));
	int rc = ove_watchdog_feed(wd);
	assert_int_equal(rc, OVE_OK);
	ove_watchdog_stop(wd);
	ove_watchdog_destroy(wd);
}

static void test_watchdog_stop(void **state)
{
	(void)state;
	ove_watchdog_t wd = NULL;
	OVE_TEST_ASSERT_OK(ove_watchdog_create(&wd, 5000));
	OVE_TEST_ASSERT_OK(ove_watchdog_start(wd));
	int rc = ove_watchdog_stop(wd);
	/* OVE_ERR_NOT_SUPPORTED is a valid return on backends whose watchdog
     * cannot be disarmed once started (STM32 IWDG is the canonical case
     * — the register set is one-way).  The public API signature allows
     * this, the test just needs to tolerate it. */
	assert_true(rc == OVE_OK || rc == OVE_ERR_NOT_SUPPORTED);
	ove_watchdog_destroy(wd);
}

#endif /* !OVE_HW */

static void test_watchdog_destroy_null(void **state)
{
	(void)state;
	ove_watchdog_destroy(NULL);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ── runner ──────────────────────────────────────────────────────────── */

int test_watchdog_run(void)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	/* Watchdog uses POSIX backend in sim — no static storage available */
	const struct CMUnitTest tests[] = {};
	(void)tests;
	printf("  [SKIP] watchdog tests (no static storage in sim zeroheap)\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_watchdog_create_destroy),
#ifndef OVE_HW
		cmocka_unit_test(test_watchdog_start),
		cmocka_unit_test(test_watchdog_feed),
		cmocka_unit_test(test_watchdog_stop),
#endif
		cmocka_unit_test(test_watchdog_destroy_null),
	};
#endif
	return cmocka_run_group_tests(tests, NULL, NULL);
}
