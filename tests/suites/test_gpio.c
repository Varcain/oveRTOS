/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "board_desc.h"

/* test_gpio_get does a read-after-write check that depends on the pin
 * not having an external pull resistor.  Reuse the LED0 pin (PI1 on
 * the STM32F746G-Discovery) — it has no external pull, so IDR follows
 * ODR — instead of hardcoding (0, 0) which on real silicon is the
 * board's WAKE button with a 10K external pull-up. */
#define TEST_GPIO_PORT OVE_LED0_PORT
#define TEST_GPIO_PIN OVE_LED0_PIN

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

static void test_gpio_set(void **state)
{
	(void)state;
	ove_board_init();
	int rc = ove_gpio_set(TEST_GPIO_PORT, TEST_GPIO_PIN, 1);
	assert_int_equal(rc, OVE_OK);
}

static void test_gpio_get(void **state)
{
	(void)state;
	ove_board_init();

	/* Real GPIO read-back requires the pin to be in OUTPUT mode so IDR
     * follows ODR.  The stub backend ignores the mode argument and the
     * Renode STM32_GPIOPort model echoes ODR via IDR regardless, so
     * this configure call is essentially a no-op there but makes the
     * test pass on real silicon. */
	ove_gpio_configure(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_MODE_OUTPUT_PP);

	ove_gpio_set(TEST_GPIO_PORT, TEST_GPIO_PIN, 1);

	int val = ove_gpio_get(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(val, 1);

	ove_gpio_set(TEST_GPIO_PORT, TEST_GPIO_PIN, 0);
	val = ove_gpio_get(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(val, 0);
}

static void test_gpio_irq_register(void **state)
{
	(void)state;
	ove_board_init();

	int rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING,
				       gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_OK);
}

static void test_gpio_irq_enable_disable(void **state)
{
	(void)state;
	ove_board_init();
	ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING, gpio_irq_handler,
			      NULL);

	int rc = ove_gpio_irq_enable(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_OK);

	rc = ove_gpio_irq_disable(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_OK);
}

static void test_gpio_set_invalid_port(void **state)
{
	(void)state;
	ove_board_init();

	int rc = ove_gpio_set(9999, 9999, 1);
	assert_int_not_equal(rc, OVE_OK);
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_gpio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_gpio_set),
		cmocka_unit_test(test_gpio_get),
		cmocka_unit_test(test_gpio_irq_register),
		cmocka_unit_test(test_gpio_irq_enable_disable),
		cmocka_unit_test(test_gpio_set_invalid_port),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
