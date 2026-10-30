/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "board_desc.h"

#ifdef OVE_TEST_STM32_EXTI
#include "ove/hal/hal_gpio.h"
#include "ove/irq.h"
#endif

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

/* Simulate a hardware GPIO interrupt: this is the exact entry point a
 * backend ISR calls to deliver to the registered ove_gpio callback.  No
 * public header declares it (it's an internal ISR hook), so forward-declare
 * it here to drive the register→enable→deliver path on the host stub, where
 * the HAL can't raise a real edge. */
extern void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin);
extern int stub_gpio_irq_is_armed(unsigned int port, unsigned int pin);
extern void stub_gpio_set_irq_unregister_result(int result);

/* Unregister the IRQ line after each IRQ test so registrations don't leak
 * across tests/suites (ove_gpio's irq_table is process-global).  That leakage
 * is why dispatch could previously fire another suite's handler on the shared
 * LED0 pin; with teardown the line is owned by exactly one test at a time, so
 * LED0 can carry the "my handler fired" + "no fire after disable" assertions. */
static int gpio_irq_teardown(void **state)
{
	(void)state;
	(void)ove_gpio_irq_unregister(TEST_GPIO_PORT, TEST_GPIO_PIN);
	return 0;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_gpio_set(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());
	int rc = ove_gpio_set(TEST_GPIO_PORT, TEST_GPIO_PIN, 1);
	assert_int_equal(rc, OVE_OK);
}

static void test_gpio_get(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	/* Real GPIO read-back requires the pin to be in OUTPUT mode so IDR
     * follows ODR.  The stub backend ignores the mode argument and the
     * Renode STM32_GPIOPort model echoes ODR via IDR regardless, so
     * this configure call is essentially a no-op there but makes the
     * test pass on real silicon. */
	OVE_TEST_ASSERT_OK(
		ove_gpio_configure(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_MODE_OUTPUT_PP));

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
	OVE_TEST_ASSERT_OK(ove_board_init());

	int rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING,
				       gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(stub_gpio_irq_is_armed(TEST_GPIO_PORT, TEST_GPIO_PIN), 0);
}

static void test_gpio_irq_rejects_invalid_mode(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	int rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, (ove_gpio_irq_mode_t)0,
				       gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_int_equal(stub_gpio_irq_is_armed(TEST_GPIO_PORT, TEST_GPIO_PIN), 0);
}

static void test_gpio_irq_rejects_null_callback(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	int rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING, NULL,
				       NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_int_equal(stub_gpio_irq_is_armed(TEST_GPIO_PORT, TEST_GPIO_PIN), 0);
}

static void test_gpio_irq_rejects_duplicate_registration(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	int rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING,
				       gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_OK);
	rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_FALLING,
				   gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_ERR_ALREADY_EXISTS);
}

static void test_gpio_irq_enable_disable(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());
	/* The gpio_irq_teardown registered on this test unregisters the line
	 * afterwards, so registrations don't leak across suites and dispatch
	 * reliably delivers to *this* file's handler — LED0 is safe to use. */
	OVE_TEST_ASSERT_OK(ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING,
						 gpio_irq_handler, NULL));

	int rc = ove_gpio_irq_enable(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_OK);

	/* Drive the real ISR dispatch path (the entry a hardware edge would hit):
	 * an enabled, registered line must deliver to the handler.  This is what
	 * makes the suite actually exercise IRQ delivery rather than only
	 * checking register/enable return codes. */
	s_irq_fired = 0;
	ove_gpio_irq_dispatch(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(s_irq_fired, 1);

	rc = ove_gpio_irq_disable(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(stub_gpio_irq_is_armed(TEST_GPIO_PORT, TEST_GPIO_PIN), 0);

	/* After disable the same dispatch must NOT reach the handler
	 * (ove_gpio_irq_dispatch gates on the per-line `enabled` flag). */
	s_irq_fired = 0;
	ove_gpio_irq_dispatch(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(s_irq_fired, 0);

	rc = ove_gpio_irq_enable(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(stub_gpio_irq_is_armed(TEST_GPIO_PORT, TEST_GPIO_PIN), 1);
	s_irq_fired = 0;
	ove_gpio_irq_dispatch(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(s_irq_fired, 1);
}

static void test_gpio_irq_retains_owner_when_unregister_fails(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	int rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING,
				       gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_OK);
	stub_gpio_set_irq_unregister_result(OVE_ERR_NOT_SUPPORTED);
	rc = ove_gpio_irq_unregister(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_ERR_NOT_SUPPORTED);
	rc = ove_gpio_irq_register(TEST_GPIO_PORT, TEST_GPIO_PIN, OVE_GPIO_IRQ_RISING,
				   gpio_irq_handler, NULL);
	assert_int_equal(rc, OVE_ERR_ALREADY_EXISTS);

	stub_gpio_set_irq_unregister_result(OVE_OK);
	rc = ove_gpio_irq_unregister(TEST_GPIO_PORT, TEST_GPIO_PIN);
	assert_int_equal(rc, OVE_OK);
}

static void test_gpio_set_invalid_port(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	int rc = ove_gpio_set(9999, 9999, 1);
	assert_int_not_equal(rc, OVE_OK);
}

static void test_gpio_irq_ops_on_unregistered_pin(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	assert_int_equal(ove_gpio_irq_enable(TEST_GPIO_PORT, TEST_GPIO_PIN), OVE_ERR_NOT_FOUND);
	assert_int_equal(ove_gpio_irq_disable(TEST_GPIO_PORT, TEST_GPIO_PIN), OVE_ERR_NOT_FOUND);
	assert_int_equal(ove_gpio_irq_unregister(TEST_GPIO_PORT, TEST_GPIO_PIN), OVE_ERR_NOT_FOUND);
}

static void test_gpio_irq_ops_invalid_pin(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	assert_int_equal(ove_gpio_irq_enable(9999, 9999), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_gpio_irq_disable(9999, 9999), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_gpio_irq_unregister(9999, 9999), OVE_ERR_INVALID_PARAM);
}

#ifdef OVE_TEST_STM32_EXTI
/* STM32: EXTI line N serves pin N of every port, one port at a time.  PI11
 * (the user button, pulled down, so safe to arm on silicon) owns line 11
 * here; PA11 is the other port's pin on it. */
#define EXTI_OWNER_PORT 8U
#define EXTI_OTHER_PORT 0U
#define EXTI_PIN 11U

static void test_gpio_irq_shared_line_one_port_at_a_time(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	/* Another port's pin on a line in use is refused, before and after
	 * the owner was armed, and gets the line once the owner is gone. */
	assert_int_equal(ove_gpio_irq_register(EXTI_OWNER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					       gpio_irq_handler, NULL),
			 OVE_OK);
	int idle = ove_gpio_irq_register(EXTI_OTHER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					 gpio_irq_handler, NULL);
	if (idle == OVE_OK)
		(void)ove_gpio_irq_unregister(EXTI_OTHER_PORT, EXTI_PIN);
	int armed = ove_gpio_irq_enable(EXTI_OWNER_PORT, EXTI_PIN);
	int owner_armed = stub_gpio_irq_is_armed(EXTI_OWNER_PORT, EXTI_PIN);
	int disarmed = ove_gpio_irq_disable(EXTI_OWNER_PORT, EXTI_PIN);
	int again = ove_gpio_irq_register(EXTI_OTHER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					  gpio_irq_handler, NULL);
	if (again == OVE_OK)
		(void)ove_gpio_irq_unregister(EXTI_OTHER_PORT, EXTI_PIN);
	(void)ove_gpio_irq_unregister(EXTI_OWNER_PORT, EXTI_PIN);

	assert_int_equal(idle, OVE_ERR_BUSY);
	assert_int_equal(armed, OVE_OK);
	assert_int_equal(owner_armed, 1);
	assert_int_equal(disarmed, OVE_OK);
	assert_int_equal(again, OVE_ERR_BUSY);

	assert_int_equal(ove_gpio_irq_register(EXTI_OTHER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					       gpio_irq_handler, NULL),
			 OVE_OK);
	assert_int_equal(ove_gpio_irq_unregister(EXTI_OTHER_PORT, EXTI_PIN), OVE_OK);
}

static void test_gpio_irq_shared_line_other_port_leaves_it_alone(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	/* With the owner holding the line, the other port's enable and
	 * disable (on the backend: the public API no longer lets it register)
	 * leave the owner's interrupt as it was. */
	assert_int_equal(ove_gpio_irq_register(EXTI_OWNER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					       gpio_irq_handler, NULL),
			 OVE_OK);
	assert_int_equal(ove_gpio_irq_enable(EXTI_OWNER_PORT, EXTI_PIN), OVE_OK);
	int off = ove_hal_gpio_irq_hw_disable(EXTI_OTHER_PORT, EXTI_PIN);
	int still_armed = stub_gpio_irq_is_armed(EXTI_OWNER_PORT, EXTI_PIN);
	(void)ove_gpio_irq_disable(EXTI_OWNER_PORT, EXTI_PIN);
	int on = ove_hal_gpio_irq_hw_enable(EXTI_OTHER_PORT, EXTI_PIN);
	int still_disarmed = !stub_gpio_irq_is_armed(EXTI_OWNER_PORT, EXTI_PIN);
	int gone = ove_hal_gpio_irq_hw_unregister(EXTI_OTHER_PORT, EXTI_PIN);
	(void)ove_gpio_irq_unregister(EXTI_OWNER_PORT, EXTI_PIN);

	assert_int_equal(off, OVE_ERR_BUSY);
	assert_int_equal(still_armed, 1);
	assert_int_equal(on, OVE_ERR_BUSY);
	assert_int_equal(still_disarmed, 1);
	assert_int_equal(gone, OVE_ERR_BUSY);
}
#define STM32F7_EXTI_SWIER (*(volatile uint32_t *)0x40013C10u)
#define NVIC_ISPR0 (*(volatile uint32_t *)0xE000E200u)
#define NVIC_ICPR0 (*(volatile uint32_t *)0xE000E280u)
#define NVIC_EXTI0 6U /* EXTI0_IRQn */

static volatile unsigned int s_irq_hits_owner;

static void owner_irq_handler(unsigned int port, unsigned int pin, void *user_data)
{
	(void)user_data;
	if (port == EXTI_OWNER_PORT && pin == EXTI_PIN)
		s_irq_hits_owner++;
}

static void test_gpio_irq_shared_line_handoff_drops_old_edge(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	/* An edge latched for the port that held the line must not reach the
	 * next holder's callback.  The edge is latched in software (SWIER)
	 * while interrupts are held off, then the line changes hands. */
	s_irq_hits_owner = 0;
	assert_int_equal(ove_gpio_irq_register(EXTI_OTHER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					       gpio_irq_handler, NULL),
			 OVE_OK);
	ove_irq_key_t key = ove_irq_lock();
	int armed = ove_gpio_irq_enable(EXTI_OTHER_PORT, EXTI_PIN);
	STM32F7_EXTI_SWIER = 1U << EXTI_PIN;
	(void)ove_gpio_irq_disable(EXTI_OTHER_PORT, EXTI_PIN);
	int released = ove_gpio_irq_unregister(EXTI_OTHER_PORT, EXTI_PIN);
	int taken = ove_gpio_irq_register(EXTI_OWNER_PORT, EXTI_PIN, OVE_GPIO_IRQ_RISING,
					  owner_irq_handler, NULL);
	int owner_armed = ove_gpio_irq_enable(EXTI_OWNER_PORT, EXTI_PIN);
	ove_irq_unlock(key);
	ove_thread_sleep_ms(10);
	unsigned int hits = s_irq_hits_owner;
	(void)ove_gpio_irq_unregister(EXTI_OWNER_PORT, EXTI_PIN);

	assert_int_equal(armed, OVE_OK);
	assert_int_equal(released, OVE_OK);
	assert_int_equal(taken, OVE_OK);
	assert_int_equal(owner_armed, OVE_OK);
	assert_int_equal(hits, 0);
}

static void test_gpio_irq_dedicated_line_handoff_drops_old_edge(void **state)
{
	(void)state;
	OVE_TEST_ASSERT_OK(ove_board_init());

	/* Lines 0-4 have interrupt vectors of their own, and an edge latched
	 * for the old holder leaves that vector pending too: registering the
	 * next holder must clear it, or the vector would run the next holder's
	 * callback.  PA0 and PI0 are floating header pins, so only the old
	 * holder is armed, and only while interrupts are held off. */
	const unsigned int pin = 0;
	const uint32_t vector = 1U << (NVIC_EXTI0 + pin);

	assert_int_equal(ove_gpio_irq_register(EXTI_OTHER_PORT, pin, OVE_GPIO_IRQ_RISING,
					       gpio_irq_handler, NULL),
			 OVE_OK);
	ove_irq_key_t key = ove_irq_lock();
	int armed = ove_gpio_irq_enable(EXTI_OTHER_PORT, pin);
	STM32F7_EXTI_SWIER = 1U << pin;
	(void)STM32F7_EXTI_SWIER; /* let the write land before looking */
	__asm__ volatile("dsb" ::: "memory");
	int latched = (NVIC_ISPR0 & vector) != 0U;
	(void)ove_gpio_irq_disable(EXTI_OTHER_PORT, pin);
	int released = ove_gpio_irq_unregister(EXTI_OTHER_PORT, pin);
	int taken = ove_gpio_irq_register(EXTI_OWNER_PORT, pin, OVE_GPIO_IRQ_RISING,
					  gpio_irq_handler, NULL);
	int still_pending = (NVIC_ISPR0 & vector) != 0U;
	(void)ove_gpio_irq_unregister(EXTI_OWNER_PORT, pin);
	NVIC_ICPR0 = vector; /* whatever the outcome, nothing is left to run */
	ove_irq_unlock(key);

	assert_int_equal(armed, OVE_OK);
	assert_int_equal(latched, 1);
	assert_int_equal(released, OVE_OK);
	assert_int_equal(taken, OVE_OK);
	assert_int_equal(still_pending, 0);
}
#endif /* OVE_TEST_STM32_EXTI */

/* ── runner ──────────────────────────────────────────────────────────── */

int test_gpio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_gpio_set),
		cmocka_unit_test(test_gpio_get),
		cmocka_unit_test_teardown(test_gpio_irq_register, gpio_irq_teardown),
		cmocka_unit_test(test_gpio_irq_rejects_invalid_mode),
		cmocka_unit_test(test_gpio_irq_rejects_null_callback),
		cmocka_unit_test_teardown(test_gpio_irq_rejects_duplicate_registration,
					  gpio_irq_teardown),
		cmocka_unit_test_teardown(test_gpio_irq_enable_disable, gpio_irq_teardown),
		cmocka_unit_test(test_gpio_irq_retains_owner_when_unregister_fails),
		cmocka_unit_test(test_gpio_set_invalid_port),
		cmocka_unit_test(test_gpio_irq_ops_on_unregistered_pin),
		cmocka_unit_test(test_gpio_irq_ops_invalid_pin),
#ifdef OVE_TEST_STM32_EXTI
		cmocka_unit_test(test_gpio_irq_shared_line_one_port_at_a_time),
		cmocka_unit_test(test_gpio_irq_shared_line_other_port_leaves_it_alone),
		cmocka_unit_test(test_gpio_irq_shared_line_handoff_drops_old_edge),
		cmocka_unit_test(test_gpio_irq_dedicated_line_handoff_drops_old_edge),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
