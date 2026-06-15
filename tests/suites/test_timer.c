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

/* userdata_cb only used by test_timer_callback_user_data which is gated
 * out under TSan; mark unused to keep -Werror=unused-function quiet. */
__attribute__((unused)) static void userdata_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	s_user_data_received = (uintptr_t)user_data;
}

/* Slow callback for the teardown-drain test: signals entry, sleeps long
 * enough that destroy is guaranteed to be called mid-execution, then
 * signals completion.  Gated like the other firing tests (TSan). */
static _Atomic int s_slow_cb_entered;
static _Atomic int s_slow_cb_completed;

__attribute__((unused)) static void slow_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
	s_slow_cb_entered = 1;
	test_msleep(80);
	s_slow_cb_completed = 1;
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

/* Timer-firing tests skipped under TSan — see test_timer_run() for the
 * rationale.  Gating the function definitions too so -Werror=unused-
 * function doesn't trip on the now-unreferenced definitions. */
#ifndef __SANITIZE_THREAD__
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
#endif /* !__SANITIZE_THREAD__ (firing tests block 1) */

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_timer_destroy_null(void **state)
{
	(void)state;
	/* Should not crash */
	ove_timer_destroy(NULL);
}
#endif

#ifndef __SANITIZE_THREAD__
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

/* Regression for the teardown UAF: destroy must block until an in-flight
 * SIGEV_THREAD callback returns, not free out from under it.  We catch the
 * callback mid-sleep, then assert destroy only returns after it completes. */
static void test_timer_destroy_waits_for_active_callback(void **state)
{
	(void)state;
	s_slow_cb_entered = 0;
	s_slow_cb_completed = 0;

	ove_timer_t t = NULL;
	ove_test_timer_create(&t, &s_tmr_storage, slow_cb, NULL, 20, 1);
	ove_timer_start(t);

	/* Wait until the callback has entered and is mid-sleep. */
	for (int i = 0; i < 1000 && !s_slow_cb_entered; i++)
		test_msleep(1);
	assert_int_equal(s_slow_cb_entered, 1);
	assert_int_equal(s_slow_cb_completed, 0);

	/* Destroy must drain the executing callback before returning. */
	ove_test_timer_destroy(t);
	assert_int_equal(s_slow_cb_completed, 1);
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
#endif /* !__SANITIZE_THREAD__ (firing tests block 2) */

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
	/* The fires_* / stop_* / reset_* / destroy_while_running /
		 * callback_user_data tests all spawn a SIGEV_THREAD per timer
		 * firing.  TSan's runtime aborts on the helper-thread stack
		 * precheck in glibc's timer dispatcher even when we size the
		 * sigev_notify_attributes stack at 256 KB; the pthread_create
		 * call inside __nptl_create_event happens with a different
		 * (smaller) stack TSan sees as too thin.  Skip the firing
		 * suite under TSan; create/destroy/null still cover the
		 * non-firing surface. */
#ifndef __SANITIZE_THREAD__
		cmocka_unit_test_setup(test_timer_oneshot_fires_once, timer_setup),
		cmocka_unit_test_setup(test_timer_periodic_fires_multiple, timer_setup),
		cmocka_unit_test_setup(test_timer_stop_prevents_callbacks, timer_setup),
		cmocka_unit_test_setup(test_timer_reset_restarts, timer_setup),
#endif
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_timer_destroy_null, timer_setup),
#endif
#ifndef __SANITIZE_THREAD__
		cmocka_unit_test_setup(test_timer_double_start, timer_setup),
		cmocka_unit_test_setup(test_timer_destroy_while_running, timer_setup),
		cmocka_unit_test_setup(test_timer_destroy_waits_for_active_callback, timer_setup),
		cmocka_unit_test_setup(test_timer_callback_user_data, timer_setup),
#endif
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_timer_create_null_handle, timer_setup),
		cmocka_unit_test_setup(test_timer_create_null_callback, timer_setup),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
