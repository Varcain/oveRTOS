/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "board_desc.h"

/* ── helpers ─────────────────────────────────────────────────────────── */

static volatile int s_irq_fired;

static void gpio_irq_handler(unsigned int port, unsigned int pin, void *user_data)
{
	(void)port;
	(void)pin;
	(void)user_data;
	s_irq_fired = 1;
}

/* Internal ISR hook the backend calls to deliver a GPIO edge to the
 * registered callback.  The BSP irq wrappers delegate to the same ove_gpio
 * irq_table, so dispatching here drives a BSP-registered handler too.  No
 * public header declares it — forward-declare to test delivery on the host. */
extern void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin);

/* Unregister the IRQ line after each IRQ test so the BSP suite's LED0
 * registrations don't leak into later suites (notably test_gpio, which shares
 * LED0) — that leakage is why a dedicated pin was previously needed. */
static int bsp_irq_teardown(void **state)
{
	(void)state;
	(void)ove_bsp_gpio_irq_unregister(OVE_LED0_PORT, OVE_LED0_PIN);
	return 0;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_bsp_board_init(void **state)
{
	(void)state;
	int rc = ove_bsp_board_init();
	assert_int_equal(rc, OVE_OK);
}

/* See test_gpio.c::test_gpio_get for the OUTPUT_PP configure rationale. */
static void test_bsp_led_set(void **state)
{
	(void)state;
	ove_bsp_board_init();
	/* No `ove_bsp_gpio_configure` shim exists in include/ove/bsp.h —
     * use ove_gpio_configure directly to put PI1 (LED0) in OUTPUT_PP
     * so the read-after-write checks below pass on real silicon. */
	ove_gpio_configure(OVE_LED0_PORT, OVE_LED0_PIN, OVE_GPIO_MODE_OUTPUT_PP);

	ove_bsp_led_set(0, 1);
	assert_int_equal(ove_bsp_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 1);
	ove_bsp_led_set(0, 0);
	assert_int_equal(ove_bsp_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 0);
}

static void test_bsp_led_toggle(void **state)
{
	(void)state;
	ove_bsp_board_init();
	/* No `ove_bsp_gpio_configure` shim exists in include/ove/bsp.h —
     * use ove_gpio_configure directly to put PI1 (LED0) in OUTPUT_PP
     * so the read-after-write checks below pass on real silicon. */
	ove_gpio_configure(OVE_LED0_PORT, OVE_LED0_PIN, OVE_GPIO_MODE_OUTPUT_PP);

	ove_bsp_led_set(0, 0);
	ove_bsp_led_toggle(0);
	assert_int_equal(ove_bsp_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 1);
	ove_bsp_led_toggle(0);
	assert_int_equal(ove_bsp_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN), 0);
}

static void test_bsp_gpio_set(void **state)
{
	(void)state;
	ove_bsp_board_init();
	int rc = ove_bsp_gpio_set(OVE_LED0_PORT, OVE_LED0_PIN, 1);
	assert_int_equal(rc, OVE_OK);
}

static void test_bsp_gpio_get(void **state)
{
	(void)state;
	ove_bsp_board_init();

	ove_bsp_gpio_set(OVE_LED0_PORT, OVE_LED0_PIN, 1);

	int val = ove_bsp_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN);
	assert_int_equal(val, 1);

	ove_bsp_gpio_set(OVE_LED0_PORT, OVE_LED0_PIN, 0);
	val = ove_bsp_gpio_get(OVE_LED0_PORT, OVE_LED0_PIN);
	assert_int_equal(val, 0);
}

static void test_bsp_gpio_irq_register(void **state)
{
	(void)state;
	ove_bsp_board_init();

	int rc = ove_bsp_gpio_irq_register(OVE_LED0_PORT, OVE_LED0_PIN, OVE_GPIO_IRQ_RISING,
					   gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_OK);
}

static void test_bsp_gpio_irq_enable_disable(void **state)
{
	(void)state;
	ove_bsp_board_init();
	/* bsp_irq_teardown unregisters the line afterwards, so registrations
	 * don't leak across suites and dispatch reliably hits this file's
	 * handler — LED0 is safe to use for the delivery + gating assertions. */
	ove_bsp_gpio_irq_register(OVE_LED0_PORT, OVE_LED0_PIN, OVE_GPIO_IRQ_RISING,
				  gpio_irq_handler, NULL);

	int rc = ove_bsp_gpio_irq_enable(OVE_LED0_PORT, OVE_LED0_PIN);
	assert_int_equal(rc, OVE_OK);

	/* Drive the real ISR dispatch path: an enabled, registered line must
	 * deliver to the handler (the BSP wrappers share ove_gpio's irq_table). */
	s_irq_fired = 0;
	ove_gpio_irq_dispatch(OVE_LED0_PORT, OVE_LED0_PIN);
	assert_int_equal(s_irq_fired, 1);

	rc = ove_bsp_gpio_irq_disable(OVE_LED0_PORT, OVE_LED0_PIN);
	assert_int_equal(rc, OVE_OK);

	/* After disable the same dispatch must NOT reach the handler. */
	s_irq_fired = 0;
	ove_gpio_irq_dispatch(OVE_LED0_PORT, OVE_LED0_PIN);
	assert_int_equal(s_irq_fired, 0);
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
		cmocka_unit_test_teardown(test_bsp_gpio_irq_register, bsp_irq_teardown),
		cmocka_unit_test_teardown(test_bsp_gpio_irq_enable_disable, bsp_irq_teardown),
		cmocka_unit_test(test_bsp_gpio_set_invalid_port),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
