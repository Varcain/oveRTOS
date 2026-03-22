/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"

/*
 * Smoke test for OVE_*_DEFINE_STATIC macros.
 * Verifies that every macro compiles and that the constructor runs
 * successfully (handle becomes non-NULL).
 */

/* --- Dummy callbacks for timer / work / thread --- */

static void dummy_timer_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
}

static void dummy_work_handler(ove_work_t work)
{
	(void)work;
}

/* --- DEFINE_STATIC declarations (file scope) --- */

OVE_MUTEX_DEFINE_STATIC(s_sd_mutex);
OVE_RECURSIVE_MUTEX_DEFINE_STATIC(s_sd_rmutex);
OVE_SEM_DEFINE_STATIC(s_sd_sem, 1, 10);
OVE_EVENT_DEFINE_STATIC(s_sd_event);
OVE_CONDVAR_DEFINE_STATIC(s_sd_condvar);
OVE_EVENTGROUP_DEFINE_STATIC(s_sd_eg);
OVE_QUEUE_DEFINE_STATIC(s_sd_queue, sizeof(int), 4);
OVE_STREAM_DEFINE_STATIC(s_sd_stream, 64, 1);
OVE_TIMER_DEFINE_STATIC(s_sd_timer, dummy_timer_cb, NULL, 1000, 1);
OVE_WATCHDOG_DEFINE_STATIC(s_sd_watchdog, 5000);
OVE_WORK_DEFINE_STATIC(s_sd_work, dummy_work_handler);

/* --- Tests --- */

static void test_static_define_mutex(void **state)
{
	(void)state;
	assert_non_null(s_sd_mutex);
	ove_mutex_lock(s_sd_mutex, OVE_WAIT_FOREVER);
	ove_mutex_unlock(s_sd_mutex);
}

static void test_static_define_recursive_mutex(void **state)
{
	(void)state;
	assert_non_null(s_sd_rmutex);
	ove_recursive_mutex_lock(s_sd_rmutex, OVE_WAIT_FOREVER);
	ove_recursive_mutex_lock(s_sd_rmutex, OVE_WAIT_FOREVER);
	ove_recursive_mutex_unlock(s_sd_rmutex);
	ove_recursive_mutex_unlock(s_sd_rmutex);
}

static void test_static_define_sem(void **state)
{
	(void)state;
	assert_non_null(s_sd_sem);
	assert_int_equal(ove_sem_take(s_sd_sem, 0), OVE_OK);
	ove_sem_give(s_sd_sem);
}

static void test_static_define_event(void **state)
{
	(void)state;
	assert_non_null(s_sd_event);
}

static void test_static_define_condvar(void **state)
{
	(void)state;
	assert_non_null(s_sd_condvar);
}

static void test_static_define_eventgroup(void **state)
{
	(void)state;
	assert_non_null(s_sd_eg);
}

static void test_static_define_queue(void **state)
{
	(void)state;
	assert_non_null(s_sd_queue);
	int val = 42;
	assert_int_equal(ove_queue_send(s_sd_queue, &val, 0), OVE_OK);
	int out = 0;
	assert_int_equal(ove_queue_receive(s_sd_queue, &out, 0), OVE_OK);
	assert_int_equal(out, 42);
}

static void test_static_define_stream(void **state)
{
	(void)state;
	assert_non_null(s_sd_stream);
}

static void test_static_define_timer(void **state)
{
	(void)state;
	assert_non_null(s_sd_timer);
}

static void test_static_define_watchdog(void **state)
{
	(void)state;
	assert_non_null(s_sd_watchdog);
}

static void test_static_define_work(void **state)
{
	(void)state;
	assert_non_null(s_sd_work);
}

/* --- Runner --- */

int test_static_define_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_static_define_mutex),
		cmocka_unit_test(test_static_define_recursive_mutex),
		cmocka_unit_test(test_static_define_sem),
		cmocka_unit_test(test_static_define_event),
		cmocka_unit_test(test_static_define_condvar),
		cmocka_unit_test(test_static_define_eventgroup),
		cmocka_unit_test(test_static_define_queue),
		cmocka_unit_test(test_static_define_stream),
		cmocka_unit_test(test_static_define_timer),
		cmocka_unit_test(test_static_define_watchdog),
		cmocka_unit_test(test_static_define_work),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
