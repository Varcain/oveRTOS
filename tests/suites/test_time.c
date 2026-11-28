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

	/* Delay must deliver at least the requested duration; tolerance is
     * OVE_TEST_TIMING_TOLERANCE_MS on the upper bound. */
	assert_true(elapsed_us >= 100000);
	assert_true(elapsed_us <= (100 + OVE_TEST_TIMING_TOLERANCE_MS) * 1000);
}

static void test_time_delay_us(void **state)
{
	(void)state;
	uint64_t before = 0, after = 0;
	ove_time_get_us(&before);
	ove_time_delay_us(10000); /* 10 ms */
	ove_time_get_us(&after);
	uint64_t elapsed_us = after - before;

	/* Delay must deliver at least the requested duration; tolerance is
     * OVE_TEST_TIMING_TOLERANCE_MS on the upper bound. */
	assert_true(elapsed_us >= 10000);
	assert_true(elapsed_us <= (10 + OVE_TEST_TIMING_TOLERANCE_MS) * 1000);
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

/* Phase 3 / Item 1: ns-resolution duration helpers — assert the
 * compile-time arithmetic so a typo in the macro definitions is
 * caught here rather than silently producing wrong timeouts. */
static void test_time_duration_helpers(void **state)
{
	(void)state;
	assert_true(OVE_NS(1) == 1ULL);
	assert_true(OVE_US(1) == 1000ULL);
	assert_true(OVE_MS(1) == 1000000ULL);
	assert_true(OVE_SEC(1) == 1000000000ULL);
	assert_true(OVE_MIN(1) == 60ULL * 1000000000ULL);
	assert_true(OVE_MS(100) == 100ULL * 1000000ULL);
	assert_true(OVE_SEC(5) == 5ULL * 1000000000ULL);
}

/* Phase 3 / Item 1: ove_time_now_steady_ns is a thin wrapper over
 * ove_time_get_ns; verify monotonic non-decrease. */
static void test_time_now_steady_ns_monotonic(void **state)
{
	(void)state;
	uint64_t prev = ove_time_now_steady_ns();
	for (int i = 0; i < 4; i++) {
		test_msleep(1);
		uint64_t now = ove_time_now_steady_ns();
		assert_true(now >= prev);
		prev = now;
	}
}

#if defined(CONFIG_OVE_ASYNC) && (defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__))
#define OVE_TEST_SYSTICK_MASKED 1
/* With interrupts masked the SysTick handler cannot run. The clock must
 * count the first reload it has missed and may stand still on later ones,
 * but must not step back. Spin masked for about 25 ms: two reloads even at
 * a 10 ms tick. */
static void test_time_ns_monotonic_irq_locked(void **state)
{
	(void)state;
	uint64_t t0, now;
	uint32_t per_ms = 0;
	for (int w = 0; w < 3; w++) {
		uint32_t n = 0;
		ove_time_get_ns(&t0);
		do {
			ove_time_get_ns(&now);
			n++;
		} while (now - t0 < OVE_MS(1));
		if (n > per_ms)
			per_ms = n;
	}

	for (int round = 0; round < 5; round++) {
		test_msleep(1);
		bool monotonic = true;
		ove_irq_key_t key = ove_irq_lock();
		ove_time_get_ns(&t0);
		uint64_t prev = t0;
		for (uint32_t i = 0; i < 25u * per_ms; i++) {
			ove_time_get_ns(&now);
			monotonic = monotonic && now >= prev;
			prev = now;
		}
		ove_irq_unlock(key);
		assert_true(monotonic);
		assert_true(prev - t0 >= OVE_MS(1));
	}
}
#endif

/* ── runner ──────────────────────────────────────────────────────────── */

int test_time_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_time_get_time_us_nonzero),
		cmocka_unit_test(test_time_monotonically_increasing),
		cmocka_unit_test(test_time_delay_ms),
		cmocka_unit_test(test_time_delay_us),
		cmocka_unit_test(test_time_passage),
		cmocka_unit_test(test_time_duration_helpers),
		cmocka_unit_test(test_time_now_steady_ns_monotonic),
#ifdef OVE_TEST_SYSTICK_MASKED
		cmocka_unit_test(test_time_ns_monotonic_irq_locked),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
