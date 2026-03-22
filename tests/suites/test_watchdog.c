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

static void test_watchdog_start(void **state)
{
    (void)state;
    ove_watchdog_t wd = NULL;
    ove_watchdog_create(&wd, 5000);
    int rc = ove_watchdog_start(wd);
    assert_int_equal(rc, OVE_OK);
    ove_watchdog_stop(wd);
    ove_watchdog_destroy(wd);
}

static void test_watchdog_feed(void **state)
{
    (void)state;
    ove_watchdog_t wd = NULL;
    ove_watchdog_create(&wd, 5000);
    ove_watchdog_start(wd);
    int rc = ove_watchdog_feed(wd);
    assert_int_equal(rc, OVE_OK);
    ove_watchdog_stop(wd);
    ove_watchdog_destroy(wd);
}

static void test_watchdog_stop(void **state)
{
    (void)state;
    ove_watchdog_t wd = NULL;
    ove_watchdog_create(&wd, 5000);
    ove_watchdog_start(wd);
    int rc = ove_watchdog_stop(wd);
    assert_int_equal(rc, OVE_OK);
    ove_watchdog_destroy(wd);
}

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
        cmocka_unit_test(test_watchdog_start),
        cmocka_unit_test(test_watchdog_feed),
        cmocka_unit_test(test_watchdog_stop),
        cmocka_unit_test(test_watchdog_destroy_null),
    };
#endif
    return cmocka_run_group_tests(tests, NULL, NULL);
}
