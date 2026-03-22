/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

/* ── helpers ─────────────────────────────────────────────────────────── */

static volatile int s_irq_fired;

static void gpio_irq_handler(unsigned int port, unsigned int pin, void *user_data)
{
    (void)port;
    (void)pin;
    (void)user_data;
    s_irq_fired = 1;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_bsp_board_init(void **state)
{
    (void)state;
    int rc = ove_bsp_board_init();
    assert_int_equal(rc, OVE_OK);
}

static void test_bsp_led_set(void **state)
{
    (void)state;
    ove_bsp_board_init();
    /* Should not crash */
    ove_bsp_led_set(0, 1);
    ove_bsp_led_set(0, 0);
}

static void test_bsp_led_toggle(void **state)
{
    (void)state;
    ove_bsp_board_init();
    /* Should not crash */
    ove_bsp_led_toggle(0);
    ove_bsp_led_toggle(0);
}

static void test_bsp_gpio_set(void **state)
{
    (void)state;
    ove_bsp_board_init();
    int rc = ove_bsp_gpio_set(0, 0, 1);
    assert_int_equal(rc, OVE_OK);
}

static void test_bsp_gpio_get(void **state)
{
    (void)state;
    ove_bsp_board_init();

    ove_bsp_gpio_set(0, 0, 1);

    int val = ove_bsp_gpio_get(0, 0);
    assert_true(val != 0);

    ove_bsp_gpio_set(0, 0, 0);
    val = ove_bsp_gpio_get(0, 0);
    assert_int_equal(val, 0);
}

static void test_bsp_gpio_irq_register(void **state)
{
    (void)state;
    ove_bsp_board_init();

    int rc = ove_bsp_gpio_irq_register(0, 0, OVE_GPIO_IRQ_RISING,
                                           gpio_irq_handler, NULL);
    assert_int_equal(rc, OVE_OK);
}

static void test_bsp_gpio_irq_enable_disable(void **state)
{
    (void)state;
    ove_bsp_board_init();
    ove_bsp_gpio_irq_register(0, 0, OVE_GPIO_IRQ_RISING,
                                  gpio_irq_handler, NULL);

    int rc = ove_bsp_gpio_irq_enable(0, 0);
    assert_int_equal(rc, OVE_OK);

    rc = ove_bsp_gpio_irq_disable(0, 0);
    assert_int_equal(rc, OVE_OK);
}

static void test_bsp_gpio_set_invalid_port(void **state)
{
    (void)state;
    ove_bsp_board_init();

    int rc = ove_bsp_gpio_set(9999, 9999, 1);
    assert_int_not_equal(rc, OVE_OK);
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_bsp_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bsp_board_init),
        cmocka_unit_test(test_bsp_led_set),
        cmocka_unit_test(test_bsp_led_toggle),
        cmocka_unit_test(test_bsp_gpio_set),
        cmocka_unit_test(test_bsp_gpio_get),
        cmocka_unit_test(test_bsp_gpio_irq_register),
        cmocka_unit_test(test_bsp_gpio_irq_enable_disable),
        cmocka_unit_test(test_bsp_gpio_set_invalid_port),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
