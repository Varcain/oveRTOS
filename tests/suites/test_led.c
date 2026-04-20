/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "board_desc.h"

static void test_led_set(void **state)
{
    (void)state;
    ove_board_init();

    /* LED 0 → port OVE_LED0_PORT, pin OVE_LED0_PIN; active_low=0 on stub. */
    ove_led_set(0, 1);
    assert_int_equal(ove_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 1);
    ove_led_set(0, 0);
    assert_int_equal(ove_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 0);
}

static void test_led_toggle(void **state)
{
    (void)state;
    ove_board_init();

    ove_led_set(0, 0);
    ove_led_toggle(0);
    assert_int_equal(ove_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 1);
    ove_led_toggle(0);
    assert_int_equal(ove_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 0);
}

static void test_led_count(void **state)
{
    (void)state;
    ove_board_init();
    assert_int_equal(ove_led_count(), OVE_LED_COUNT);
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
