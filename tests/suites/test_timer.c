/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_timer_storage_t, s_tmr_storage);

/* ── helpers ─────────────────────────────────────────────────────────── */

static _Atomic int s_oneshot_count;
static _Atomic int s_periodic_count;
static _Atomic uintptr_t s_user_data_received;

static void oneshot_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_oneshot_count++;
}

static void periodic_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_periodic_count++;
}

static void userdata_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	s_user_data_received = (uintptr_t)user_data;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_timer_create_destroy_oneshot(void **state)
{
	(void)state;
	ove_timer_t t = NULL;
	int rc = ove_test_timer_create(&t, &s_tmr_storage, oneshot_cb, NULL, 100, 1);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(t);
	ove_test_timer_destroy(t);
}

static void test_timer_create_destroy_periodic(void **state)
{
	(void)state;
	ove_timer_t t = NULL;
	int rc = ove_test_timer_create(&t, &s_tmr_storage, periodic_cb, NULL, 50, 0);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(t);
	ove_test_timer_destroy(t);
}

static void test_timer_oneshot_fires_once(void **state)
{
	(void)state;
	s_oneshot_count = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, oneshot_cb, NULL, 30, 1);
	ove_timer_start(t);

	test_msleep(200); /* 200 ms — plenty of time */

	assert_int_equal(s_oneshot_count, 1);

	ove_test_timer_destroy(t);
}

static void test_timer_periodic_fires_multiple(void **state)
{
	(void)state;
	s_periodic_count = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, periodic_cb, NULL, 30, 0);
	ove_timer_start(t);

	test_msleep(250); /* 250 ms — should get ~8 fires at 30 ms period */

	ove_timer_stop(t);

	/* Expected ~8 fires; allow 3–20 for scheduler jitter but cap to catch
     * runaway timers. */
	assert_true(s_periodic_count >= 3);
	assert_true(s_periodic_count <= 20);

	ove_test_timer_destroy(t);
}

static void test_timer_stop_prevents_callbacks(void **state)
{
	(void)state;
	s_periodic_count = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, periodic_cb, NULL, 20, 0);
	ove_timer_start(t);

	test_msleep(100);
	ove_timer_stop(t);

	int count_after_stop = s_periodic_count;
	test_msleep(150);

	/* At most 1 in-flight callback may land after stop (SIGEV_THREAD race) */
	assert_true(s_periodic_count <= count_after_stop + 1);

	ove_test_timer_destroy(t);
}

static void test_timer_reset_restarts(void **state)
{
	(void)state;
	s_periodic_count = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, periodic_cb, NULL, 50, 0);
	ove_timer_start(t);

	test_msleep(80);
	int before_reset = s_periodic_count;
	ove_timer_reset(t);

	test_msleep(200);

	/* After reset the timer keeps running; we should see more callbacks */
	assert_true(s_periodic_count > before_reset);

	ove_timer_stop(t);
	ove_test_timer_destroy(t);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_timer_destroy_null(void **state)
{
	(void)state;
	/* Should not crash */
	ove_timer_destroy(NULL);
}
#endif

static void test_timer_double_start(void **state)
{
	(void)state;
	s_periodic_count = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, periodic_cb, NULL, 30, 0);
	ove_timer_start(t);
	/* Start again while running — should not crash */
	ove_timer_start(t);

	test_msleep(150);

	ove_timer_stop(t);
	assert_true(s_periodic_count >= 2);

	ove_test_timer_destroy(t);
}

static void test_timer_destroy_while_running(void **state)
{
	(void)state;
	s_periodic_count = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, periodic_cb, NULL, 20, 0);
	ove_timer_start(t);

	test_msleep(60);

	/* Destroy without stop — should not crash or leak.
     * The backend disarms the timer and drains in-flight callbacks. */
	ove_test_timer_destroy(t);
}

static void test_timer_callback_user_data(void **state)
{
	(void)state;
	s_user_data_received = 0;

	uintptr_t magic = 0xDEADBEEF;
	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, userdata_cb, (void *)magic, 20, 1);
	ove_timer_start(t);

	test_msleep(150);

	assert_int_equal(s_user_data_received, magic);

	ove_test_timer_destroy(t);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_timer_create_null_handle(void **state)
{
	(void)state;
	int rc = ove_timer_create(NULL, periodic_cb, NULL, 100, 0);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_timer_create_null_callback(void **state)
{
	(void)state;
	ove_timer_t t = NULL;
	int rc = ove_timer_create(&t, NULL, NULL, 100, 0);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}
#endif

/* ── setup/teardown ──────────────────────────────────────────────────── */

static int timer_setup(void **state)
{
	(void)state;
	s_oneshot_count = 0;
	s_periodic_count = 0;
	s_user_data_received = 0;
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_timer_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_timer_create_destroy_oneshot, timer_setup),
		cmocka_unit_test_setup(test_timer_create_destroy_periodic, timer_setup),
		cmocka_unit_test_setup(test_timer_oneshot_fires_once, timer_setup),
		cmocka_unit_test_setup(test_timer_periodic_fires_multiple, timer_setup),
		cmocka_unit_test_setup(test_timer_stop_prevents_callbacks, timer_setup),
		cmocka_unit_test_setup(test_timer_reset_restarts, timer_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_timer_destroy_null, timer_setup),
#endif
		cmocka_unit_test_setup(test_timer_double_start, timer_setup),
		cmocka_unit_test_setup(test_timer_destroy_while_running, timer_setup),
		cmocka_unit_test_setup(test_timer_callback_user_data, timer_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_timer_create_null_handle, timer_setup),
		cmocka_unit_test_setup(test_timer_create_null_callback, timer_setup),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
