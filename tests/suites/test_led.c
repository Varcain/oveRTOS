/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

static void test_led_set(void **state)
{
    (void)state;
    ove_board_init();
    /* Should not crash */
    ove_led_set(0, 1);
    ove_led_set(0, 0);
}

static void test_led_toggle(void **state)
{
    (void)state;
    ove_board_init();
    /* Should not crash */
    ove_led_toggle(0);
    ove_led_toggle(0);
}

static void test_led_count(void **state)
{
    (void)state;
    ove_board_init();
    unsigned int count = ove_led_count();
    assert_true(count > 0);
}

int test_led_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_led_set),
        cmocka_unit_test(test_led_toggle),
        cmocka_unit_test(test_led_count),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
