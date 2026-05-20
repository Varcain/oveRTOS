/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Cmocka tests for the CONFIG_OVE_ASYNC C substrate that the Rust
 * binding's embassy-time driver sits on top of:
 *
 *   - ove_irq_lock / ove_irq_unlock / ove_is_in_isr (include/ove/irq.h)
 *   - ove_timer_init_ns / ove_timer_create_ns / ove_timer_set_period_ns
 *     (include/ove/timer.h, gated by CONFIG_OVE_ASYNC for the new
 *     symbols — only ove_timer_init_ns / _create_ns are also available
 *     as gated extensions to the existing timer API).
 *
 * The end-to-end Rust async stack (embassy executor + ove::async_runtime)
 * is exercised by the apps/rust/{heap,zeroheap}/example_async/ demos
 * we run manually under QEMU / on hardware; these tests are the C-side
 * regression gate.
 *
 * When CONFIG_OVE_ASYNC is off the file shrinks to a stub
 * `test_async_run` that returns 0, so the suite list in suites.inc
 * stays consistent across backend configurations.
 */

#include "../framework/ove_test.h"

#ifdef CONFIG_OVE_ASYNC

#include "ove/irq.h"
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_timer_storage_t, s_async_tmr_storage);

/* ── helpers ─────────────────────────────────────────────────────────── */

static _Atomic int s_async_fire_count;
/* 64-bit timestamp: `volatile` rather than `_Atomic` so the test
 * compiles on 32-bit ARM (no native 64-bit atomic — libatomic isn't
 * linked into Zephyr/picolibc).  Reads / writes don't tear in practice
 * because the test thread always sleeps between arming the timer and
 * reading the field, so the callback's store is fully ordered behind
 * the wake of the test thread. */
static volatile uint64_t s_async_fire_us;

static void async_fire_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_async_fire_count++;
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	s_async_fire_us = now;
}

/* ── ove_irq_lock / unlock / is_in_isr ──────────────────────────────── */

static void test_async_irq_lock_unlock_roundtrip(void **state)
{
	(void)state;
	ove_irq_key_t key = ove_irq_lock();
	/* Nothing observable about the cookie itself across backends — the
	 * contract is "pass this exact value back to ove_irq_unlock". */
	ove_irq_unlock(key);
}

static void test_async_irq_lock_nested(void **state)
{
	(void)state;
	/* LIFO nesting: outer + inner pair, no fault, no deadlock. */
	ove_irq_key_t k1 = ove_irq_lock();
	ove_irq_key_t k2 = ove_irq_lock();
	ove_irq_unlock(k2);
	ove_irq_unlock(k1);
}

static void test_async_is_in_isr_thread_context(void **state)
{
	(void)state;
	/* This test runs from the cmocka main thread (or its delegate
	 * test task on FreeRTOS/Zephyr/NuttX) — never an ISR. */
	assert_false(ove_is_in_isr());
}

/* ── ove_timer_*_ns: create / fire / reprogram ──────────────────────── */
/*
 * Timer-firing tests use SIGEV_THREAD on POSIX/NuttX (one helper pthread
 * per timer expiry).  TSan's runtime aborts on the helper-thread stack
 * precheck even when sigev_notify_attributes provides a 256 KB stack
 * (see test_timer.c for the full notes), so all firing tests are
 * gated out under __SANITIZE_THREAD__.
 */

#ifndef __SANITIZE_THREAD__

static void test_async_timer_init_ns_fires(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_test_timer_create_ns(&t, &s_async_tmr_storage, async_fire_cb, NULL,
					  10000000ULL /* 10 ms */, 1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);

	rc = ove_timer_start(t);
	assert_int_equal(rc, OVE_OK);

	test_msleep(80); /* generous window for the slowest backend (FreeRTOS 1 ms tick) */

	assert_int_equal(s_async_fire_count, 1);

	ove_test_timer_destroy(t);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_async_timer_create_ns_heap(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_timer_create_ns(&t, async_fire_cb, NULL, 10000000ULL /* 10 ms */,
				     1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(t);

	rc = ove_timer_start(t);
	assert_int_equal(rc, OVE_OK);

	test_msleep(80);
	assert_int_equal(s_async_fire_count, 1);

	ove_timer_destroy(t);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

/* ── The regression that motivated ove_timer_set_period_ns ─────────────
 *
 * The Embassy time driver re-arms its global alarm at every
 * schedule_wake.  The naive implementation (stop + init_ns again)
 * corrupted FreeRTOS's daemon-task list because xTimerCreateStatic was
 * called repeatedly on the same `static_timer` slot — the first alarm
 * fired, every subsequent one didn't.  This test catches the regression
 * directly: arm a one-shot, wait for it to fire, then arm it again via
 * set_period_ns and assert the second fire actually happens.
 */
static void test_async_timer_set_period_ns_rearms_after_fire(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_test_timer_create_ns(&t, &s_async_tmr_storage, async_fire_cb, NULL,
					  10000000ULL /* 10 ms */, 1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);

	assert_int_equal(ove_timer_start(t), OVE_OK);
	test_msleep(80);
	assert_int_equal(s_async_fire_count, 1);

	/* Re-arm via set_period_ns — must trigger a second fire. */
	rc = ove_timer_set_period_ns(t, 10000000ULL);
	assert_int_equal(rc, OVE_OK);

	test_msleep(80);
	assert_int_equal(s_async_fire_count, 2);

	ove_test_timer_destroy(t);
}

/* set_period_ns on a running timer should restart the countdown with
 * the new period and not let the original period fire first. */
static void test_async_timer_set_period_ns_reprograms_running(void **state)
{
	(void)state;
	s_async_fire_count = 0;

	ove_timer_t t = NULL;
	int rc = ove_test_timer_create_ns(&t, &s_async_tmr_storage, async_fire_cb, NULL,
					  200000000ULL /* 200 ms */, 1 /* one_shot */);
	assert_int_equal(rc, OVE_OK);

	uint64_t arm_us = 0;
	(void)ove_time_get_us(&arm_us);
	assert_int_equal(ove_timer_start(t), OVE_OK);

	/* Reprogram to 30 ms before the original 200 ms deadline. */
	test_msleep(10);
	rc = ove_timer_set_period_ns(t, 30000000ULL /* 30 ms */);
	assert_int_equal(rc, OVE_OK);

	test_msleep(100);
	assert_int_equal(s_async_fire_count, 1);

	/* The fire should land closer to the reprogrammed 30 ms than the
	 * original 200 ms — give the slowest backend (FreeRTOS 1 ms tick
	 * + workqueue dispatch) up to 100 ms of slack. */
	uint64_t elapsed = s_async_fire_us - arm_us;
	assert_true(elapsed < 150000ULL); /* < 150 ms — well under the 200 ms original */

	ove_test_timer_destroy(t);
}

#endif /* !__SANITIZE_THREAD__ */

/* ── setup ───────────────────────────────────────────────────────────── */

static int async_setup(void **state)
{
	(void)state;
	s_async_fire_count = 0;
	s_async_fire_us = 0;
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_async_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_async_irq_lock_unlock_roundtrip, async_setup),
		cmocka_unit_test_setup(test_async_irq_lock_nested, async_setup),
		cmocka_unit_test_setup(test_async_is_in_isr_thread_context, async_setup),
#ifndef __SANITIZE_THREAD__
		cmocka_unit_test_setup(test_async_timer_init_ns_fires, async_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_async_timer_create_ns_heap, async_setup),
#endif
		cmocka_unit_test_setup(test_async_timer_set_period_ns_rearms_after_fire,
				       async_setup),
		cmocka_unit_test_setup(test_async_timer_set_period_ns_reprograms_running,
				       async_setup),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* !CONFIG_OVE_ASYNC */

int test_async_run(void)
{
	/* CONFIG_OVE_ASYNC=n: no symbols to test.  Stub keeps the suite
	 * list (framework/suites.inc) consistent across backend configs. */
	return 0;
}

#endif /* CONFIG_OVE_ASYNC */
