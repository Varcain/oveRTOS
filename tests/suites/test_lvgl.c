/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_lvgl_init(void **state)
{
    (void)state;
    int rc = ove_lvgl_init();
    assert_int_equal(rc, OVE_OK);
}

static void test_lvgl_lock_unlock(void **state)
{
    (void)state;
    ove_lvgl_init();
    /* lock/unlock is a no-op on the stub backend; exercised here to
     * catch linkage regressions and symbol drift. */
    ove_lvgl_lock();
    ove_lvgl_unlock();
}

static void test_lvgl_tick(void **state)
{
    (void)state;
    ove_lvgl_init();
    ove_lvgl_tick(10);
}

static void test_lvgl_handler(void **state)
{
    (void)state;
    ove_lvgl_init();
    ove_lvgl_handler();
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_lvgl_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_lvgl_init),
        cmocka_unit_test(test_lvgl_lock_unlock),
        cmocka_unit_test(test_lvgl_tick),
        cmocka_unit_test(test_lvgl_handler),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
