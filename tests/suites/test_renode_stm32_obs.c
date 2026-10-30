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
#include "ove/hal/hal_gpio.h"
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
	case 0:
		return GPIOA;
	case 1:
		return GPIOB;
	case 2:
		return GPIOC;
	case 3:
		return GPIOD;
	case 4:
		return GPIOE;
	case 5:
		return GPIOF;
	case 6:
		return GPIOG;
	case 7:
		return GPIOH;
	case 8:
		return GPIOI;
	default:
		return NULL;
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
	const unsigned int pin = ove_board_leds[0].pin;
	const int active_low = ove_board_leds[0].active_low;
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
	const unsigned int pin = 5; /* PA5 — no shared use on Discovery */

	int rc = ove_gpio_configure(PORT_A, pin, OVE_GPIO_MODE_OUTPUT_PP);
	assert_int_equal(rc, OVE_OK);

	ove_gpio_set(PORT_A, pin, 0);
	assert_int_equal(ove_obs_read32((uintptr_t)&GPIOA->ODR) & (1U << pin), 0U);

	ove_gpio_set(PORT_A, pin, 1);
	assert_int_equal(ove_obs_read32((uintptr_t)&GPIOA->ODR) & (1U << pin), (1U << pin));

	ove_gpio_set(PORT_A, pin, 0);
}

/* ── Test 3: external trigger → ove_gpio IRQ callback ──────────────── */

static volatile int g_irq_fired;

static void external_irq_handler(unsigned int port, unsigned int pin, void *user_data)
{
	(void)user_data;
	if (port == PORT_A && pin == 0) {
		(void)__atomic_fetch_add(&g_irq_fired, 1, __ATOMIC_RELEASE);
	}
}

static void test_external_irq_trigger(void **state)
{
	(void)state;

	/* Configure PA0 as EXTI rising-edge input via the real ove_gpio
	 * API. */
	g_irq_fired = 0;
	int rc = ove_gpio_irq_register(PORT_A, 0, OVE_GPIO_IRQ_RISING, external_irq_handler, NULL);
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
	(void)ove_gpio_irq_unregister(PORT_A, 0);
}

static void noop_irq_handler(unsigned int port, unsigned int pin, void *user_data)
{
	(void)port;
	(void)pin;
	(void)user_data;
}

static void test_disabling_exti_line_preserves_shared_vector_peer(void **state)
{
	(void)state;
	const unsigned int first_pin = 5;
	const unsigned int peer_pin = 6;

	assert_int_equal(ove_gpio_irq_register(PORT_A, first_pin, OVE_GPIO_IRQ_RISING,
					       noop_irq_handler, NULL),
			 OVE_OK);
	assert_int_equal(ove_gpio_irq_register(PORT_A, peer_pin, OVE_GPIO_IRQ_RISING,
					       noop_irq_handler, NULL),
			 OVE_OK);
	assert_int_equal(ove_gpio_irq_enable(PORT_A, first_pin), OVE_OK);
	assert_int_equal(ove_gpio_irq_enable(PORT_A, peer_pin), OVE_OK);
	assert_int_equal(ove_gpio_irq_disable(PORT_A, first_pin), OVE_OK);

	assert_int_equal(ove_obs_read32((uintptr_t)&EXTI->IMR) & (1U << first_pin), 0U);
	assert_int_equal(ove_obs_read32((uintptr_t)&EXTI->IMR) & (1U << peer_pin),
			 (1U << peer_pin));

	(void)ove_gpio_irq_unregister(PORT_A, first_pin);
	(void)ove_gpio_irq_unregister(PORT_A, peer_pin);
}

/* ── EXTI lines shared between ports ───────────────────────────────── */

/* EXTI line N serves pin N of every port, routed to one port at a time by
 * SYSCFG.  PI11 is the board's user button, pulled down, so arming it is
 * safe on silicon; PA11 is the other port's pin on the same line. */
#define PORT_I 8
#define SHARED_PIN 11

/* The port EXTI line @p pin is routed to. */
static unsigned int exti_route(unsigned int pin)
{
	return (ove_obs_read32((uintptr_t)&SYSCFG->EXTICR[pin / 4U]) >> (4U * (pin % 4U))) & 0xFU;
}

static void test_exti_line_one_port_at_a_time(void **state)
{
	(void)state;

	/* A second port's pin on a line in use is refused.  It used to
	 * re-route the line, and the first port's edges were lost. */
	assert_int_equal(ove_gpio_irq_register(PORT_I, SHARED_PIN, OVE_GPIO_IRQ_RISING,
					       noop_irq_handler, NULL),
			 OVE_OK);
	int idle = ove_gpio_irq_register(PORT_A, SHARED_PIN, OVE_GPIO_IRQ_RISING, noop_irq_handler,
					 NULL);
	unsigned int idle_route = exti_route(SHARED_PIN);
	if (idle == OVE_OK)
		(void)ove_gpio_irq_unregister(PORT_A, SHARED_PIN);

	/* The same after the owner was armed and disarmed again. */
	int armed = ove_gpio_irq_enable(PORT_I, SHARED_PIN);
	int disarmed = ove_gpio_irq_disable(PORT_I, SHARED_PIN);
	int again = ove_gpio_irq_register(PORT_A, SHARED_PIN, OVE_GPIO_IRQ_RISING, noop_irq_handler,
					  NULL);
	unsigned int again_route = exti_route(SHARED_PIN);
	if (again == OVE_OK)
		(void)ove_gpio_irq_unregister(PORT_A, SHARED_PIN);
	(void)ove_gpio_irq_unregister(PORT_I, SHARED_PIN);

	assert_int_equal(idle, OVE_ERR_BUSY);
	assert_int_equal(idle_route, PORT_I);
	assert_int_equal(armed, OVE_OK);
	assert_int_equal(disarmed, OVE_OK);
	assert_int_equal(again, OVE_ERR_BUSY);
	assert_int_equal(again_route, PORT_I);

	/* Once the owner is gone, the other port can have the line. */
	assert_int_equal(ove_gpio_irq_register(PORT_A, SHARED_PIN, OVE_GPIO_IRQ_RISING,
					       noop_irq_handler, NULL),
			 OVE_OK);
	assert_int_equal(exti_route(SHARED_PIN), PORT_A);
	assert_int_equal(ove_gpio_irq_unregister(PORT_A, SHARED_PIN), OVE_OK);
}

/* In DTCM: main RAM has no room left. */
__attribute__((section(".dsp_bss"))) static unsigned int g_exti_port;
__attribute__((section(".dsp_bss"))) static unsigned int g_exti_hits;

static void record_irq(unsigned int port, unsigned int pin, void *user_data)
{
	(void)user_data;
	if (pin == SHARED_PIN) {
		g_exti_port = port;
		(void)__atomic_fetch_add(&g_exti_hits, 1, __ATOMIC_RELEASE);
	}
}

static void test_exti_dispatch_names_the_routed_port(void **state)
{
	(void)state;

	/* The EXTI vector reports only the line; the port is the one the line
	 * is routed to.  It used to be reported as port A whatever the route,
	 * so a callback on any other port never ran. */
	g_exti_port = 0xFFU;
	g_exti_hits = 0;
	assert_int_equal(ove_gpio_irq_register(PORT_I, SHARED_PIN, OVE_GPIO_IRQ_RISING, record_irq,
					       NULL),
			 OVE_OK);
	assert_int_equal(ove_gpio_irq_enable(PORT_I, SHARED_PIN), OVE_OK);
	HAL_GPIO_EXTI_Callback((uint16_t)(1U << SHARED_PIN)); /* as the EXTI vector does */
	(void)ove_gpio_irq_disable(PORT_I, SHARED_PIN);
	(void)ove_gpio_irq_unregister(PORT_I, SHARED_PIN);

	assert_int_equal(__atomic_load_n(&g_exti_hits, __ATOMIC_ACQUIRE), 1);
	assert_int_equal(g_exti_port, PORT_I);
}

static void test_exti_other_port_leaves_line_alone(void **state)
{
	(void)state;
	const uint32_t bit = 1U << SHARED_PIN;

	/* With PI11 owning the line, PA11's enable and disable must not touch
	 * the line's mask.  Called on the backend: the public API no longer
	 * lets PA11 register while PI11 holds the line. */
	assert_int_equal(ove_gpio_irq_register(PORT_I, SHARED_PIN, OVE_GPIO_IRQ_RISING,
					       noop_irq_handler, NULL),
			 OVE_OK);
	assert_int_equal(ove_gpio_irq_enable(PORT_I, SHARED_PIN), OVE_OK);
	int off = ove_hal_gpio_irq_hw_disable(PORT_A, SHARED_PIN);
	uint32_t after_off = ove_obs_read32((uintptr_t)&EXTI->IMR) & bit;
	(void)ove_gpio_irq_disable(PORT_I, SHARED_PIN);
	int on = ove_hal_gpio_irq_hw_enable(PORT_A, SHARED_PIN);
	uint32_t after_on = ove_obs_read32((uintptr_t)&EXTI->IMR) & bit;
	(void)ove_gpio_irq_unregister(PORT_I, SHARED_PIN);

	assert_int_equal(off, OVE_ERR_BUSY);
	assert_int_equal(after_off, bit);
	assert_int_equal(on, OVE_ERR_BUSY);
	assert_int_equal(after_on, 0U);
}

#endif /* OVE_OBS_AVAILABLE */

/* ── Runner ────────────────────────────────────────────────────────── */

int test_renode_stm32_obs_run(void)
{
#if !OVE_OBS_AVAILABLE
	/* OVE_OBS_AVAILABLE is 0 on genuinely non-Renode targets (QEMU/POSIX)
	 * AND on the NuttX/Zephyr Renode targets, which run under Renode but do
	 * not yet wire up the STM32 HAL register layer (only the FreeRTOS Renode
	 * build defines OVE_RENODE_STM32F746).  Word the skip so it does not
	 * falsely claim "non-Renode" on those Renode backends.  See
	 * framework/renode_obs.h for the gap + what enabling them requires. */
	printf("  [SKIP] renode_stm32_obs — register observation not wired here "
	       "(non-Renode target, or NuttX/Zephyr-Renode w/o the STM32 HAL register layer)\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_led_set_observable_in_odr),
		cmocka_unit_test(test_gpio_set_observable_in_odr),
		cmocka_unit_test(test_exti_line_one_port_at_a_time),
		cmocka_unit_test(test_exti_dispatch_names_the_routed_port),
		cmocka_unit_test(test_exti_other_port_leaves_line_alone),
#ifndef OVE_HW
		/* PA5/PA6 are unpulled header pins on the Discovery board: on
		 * silicon they float, and their armed rising edges storm the
		 * shared EXTI9_5 vector so the test thread never runs again. */
		cmocka_unit_test(test_disabling_exti_line_preserves_shared_vector_peer),
		/* test_external_irq_trigger needs a host-side stimulus
		 * (`sysbus.gpioPortA OnGPIO 0 true` in test.resc) — there's
		 * no equivalent injection on real silicon, so skip on HW. */
		cmocka_unit_test(test_external_irq_trigger),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
