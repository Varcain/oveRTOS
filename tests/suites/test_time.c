/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"


/* ── tests ───────────────────────────────────────────────────────────── */

static void test_time_get_time_us_nonzero(void **state)
{
    (void)state;
    uint64_t t = 0;
    int rc = ove_time_get_us(&t);
    assert_int_equal(rc, OVE_OK);
    assert_true(t > 0);
}

static void test_time_monotonically_increasing(void **state)
{
    (void)state;
    uint64_t t1 = 0, t2 = 0;
    ove_time_get_us(&t1);
    test_msleep(1);
    ove_time_get_us(&t2);
    assert_true(t2 > t1);
}

static void test_time_delay_ms(void **state)
{
    (void)state;
    uint64_t before = 0, after = 0;
    ove_time_get_us(&before);
    ove_time_delay_ms(100);
    ove_time_get_us(&after);
    uint64_t elapsed_us = after - before;

    /* Should be at least 90 ms and no more than 200 ms */
    assert_true(elapsed_us >= 90000);
    assert_true(elapsed_us <= 200000);
}

static void test_time_delay_us(void **state)
{
    (void)state;
    uint64_t before = 0, after = 0;
    ove_time_get_us(&before);
    ove_time_delay_us(10000); /* 10 ms */
    ove_time_get_us(&after);
    uint64_t elapsed_us = after - before;

    /* Should be at least 8 ms and no more than 50 ms */
    assert_true(elapsed_us >= 8000);
    assert_true(elapsed_us <= 50000);
}

static void test_time_passage(void **state)
{
    (void)state;
    uint64_t samples[5];
    for (int i = 0; i < 5; i++) {
        ove_time_get_us(&samples[i]);
        test_msleep(1);
    }
    for (int i = 1; i < 5; i++) {
        assert_true(samples[i] > samples[i - 1]);
    }
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_time_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_time_get_time_us_nonzero),
        cmocka_unit_test(test_time_monotonically_increasing),
        cmocka_unit_test(test_time_delay_ms),
        cmocka_unit_test(test_time_delay_us),
        cmocka_unit_test(test_time_passage),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
