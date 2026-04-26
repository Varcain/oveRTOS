/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Renode-target observability suite — verifies oveRTOS APIs by checking
 * the underlying STM32 peripheral register state Renode models.
 *
 * On QEMU/POSIX/sim targets the whole suite is skipped (the macros in
 * framework/renode_obs.h compile to zero-information stubs and
 * OVE_OBS_AVAILABLE is 0).  On Renode the tests:
 *   1. Verify ove_led_on(0) actually drives PI1 high (the
 *      stm32f746g-discovery user LED maps there).
 *   2. Verify ove_gpio_set arrives at the corresponding ODR bit.
 *   3. Verify an EXTI line driven from the harness fires the
 *      registered ove_gpio_irq callback end-to-end.
 */

#include "../framework/ove_test.h"
#include "../framework/renode_obs.h"
#include "ove/gpio.h"
#include "ove/led.h"
#include "board_desc.h"

#include <stdio.h>

#if OVE_OBS_AVAILABLE

/* ── Helpers ───────────────────────────────────────────────────────── */

#define PORT_A 0

/* Resolve a port number (board_desc convention: 0..8 → A..I) to the
 * matching CMSIS GPIO_TypeDef pointer Renode models. */
static GPIO_TypeDef *port_index_to_gpio(unsigned int port)
{
	switch (port) {
	case 0: return GPIOA;
	case 1: return GPIOB;
	case 2: return GPIOC;
	case 3: return GPIOD;
	case 4: return GPIOE;
	case 5: return GPIOF;
	case 6: return GPIOG;
	case 7: return GPIOH;
	case 8: return GPIOI;
	default: return NULL;
	}
}

/* ── Test 1: ove_led_set arrives at the board-configured GPIO pin ──── */

static void test_led_set_observable_in_odr(void **state)
{
	(void)state;

	if (ove_led_count() == 0) {
		print_message("  [skip] ove_led_count() == 0\n");
		return;
	}
	const unsigned int port = ove_board_leds[0].port;
	const unsigned int pin  = ove_board_leds[0].pin;
	const int active_low    = ove_board_leds[0].active_low;
	GPIO_TypeDef *gpio = port_index_to_gpio(port);
	assert_non_null(gpio);

	/* Test firmware uses a stub board, so configure the LED pin
	 * ourselves before driving it via the API. */
	int rc = ove_gpio_configure(port, pin, OVE_GPIO_MODE_OUTPUT_PP);
	assert_int_equal(rc, OVE_OK);

	const int high_when_on = active_low ? 0 : 1;

	ove_led_set(0, 0);
	assert_int_equal((int)OVE_OBS_GPIO_PIN_HIGH(gpio, pin), !high_when_on);

	ove_led_set(0, 1);
	assert_int_equal((int)OVE_OBS_GPIO_PIN_HIGH(gpio, pin), high_when_on);

	ove_led_set(0, 0);
	assert_int_equal((int)OVE_OBS_GPIO_PIN_HIGH(gpio, pin), !high_when_on);
}

/* ── Test 2: ove_gpio_set observable in ODR ────────────────────────── */

static void test_gpio_set_observable_in_odr(void **state)
{
	(void)state;
	const unsigned int pin = 5;  /* PA5 — no shared use on Discovery */

	int rc = ove_gpio_configure(PORT_A, pin, OVE_GPIO_MODE_OUTPUT_PP);
	assert_int_equal(rc, OVE_OK);

	ove_gpio_set(PORT_A, pin, 0);
	assert_int_equal(ove_obs_read32((uintptr_t)&GPIOA->ODR) & (1U << pin),
			 0U);

	ove_gpio_set(PORT_A, pin, 1);
	assert_int_equal(ove_obs_read32((uintptr_t)&GPIOA->ODR) & (1U << pin),
			 (1U << pin));

	ove_gpio_set(PORT_A, pin, 0);
}

/* ── Test 3: external trigger → ove_gpio IRQ callback ──────────────── */

static volatile int g_irq_fired;

static void external_irq_handler(unsigned int port, unsigned int pin,
				  void *user_data)
{
	(void)user_data;
	if (port == PORT_A && pin == 0) {
		g_irq_fired += 1;
	}
}

static void test_external_irq_trigger(void **state)
{
	(void)state;

	/* Configure PA0 as EXTI rising-edge input via the real ove_gpio
	 * API.  The ove_board_gpio_exti_port weak default returns port 0
	 * (PORT_A), which matches what we want. */
	g_irq_fired = 0;
	int rc = ove_gpio_irq_register(PORT_A, 0, OVE_GPIO_IRQ_RISING,
				       external_irq_handler, NULL);
	assert_int_equal(rc, OVE_OK);
	rc = ove_gpio_irq_enable(PORT_A, 0);
	assert_int_equal(rc, OVE_OK);

	/* test.resc schedules `sysbus.gpioPortA OnGPIO 0 true` to fire
	 * ~1 simulated second after machine start.  Bound the wait
	 * generously — Renode's wall-clock-to-sim-clock ratio drifts
	 * with host load. */
	wait_for_flag(&g_irq_fired, 1, 5000);
	assert_int_not_equal(g_irq_fired, 0);

	ove_gpio_irq_disable(PORT_A, 0);
}

#endif /* OVE_OBS_AVAILABLE */

/* ── Runner ────────────────────────────────────────────────────────── */

int test_renode_stm32_obs_run(void)
{
#if !OVE_OBS_AVAILABLE
	printf("  [SKIP] renode_stm32_obs — non-Renode target\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_led_set_observable_in_odr),
		cmocka_unit_test(test_gpio_set_observable_in_odr),
		cmocka_unit_test(test_external_irq_trigger),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
