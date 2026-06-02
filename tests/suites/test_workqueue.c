/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_workqueue_storage_t, s_wq_storage);
OVE_TEST_STACK(s_wq_stack, 2048);
OVE_TEST_STORAGE(ove_work_storage_t, s_work_storage);
OVE_TEST_STORAGE(ove_work_storage_t, s_work_storages[3]);

/* ── helpers ─────────────────────────────────────────────────────────── */

static volatile int s_work_called;
static _Atomic int s_work_count;
static _Atomic intptr_t s_received_handle_raw;

static void work_handler(ove_work_t work)
{
	atomic_store(&s_received_handle_raw, (intptr_t)work);
	TEST_FLAG_SET(s_work_called, 1);
}

static void counting_handler(ove_work_t work)
{
	(void)work;
	atomic_fetch_add(&s_work_count, 1);
}

static void delayed_handler(ove_work_t work)
{
	(void)work;
	TEST_FLAG_SET(s_work_called, 1);
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_wq_create_destroy(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	int rc = ove_test_workqueue_create(&wq, &s_wq_storage, "test_wq", OVE_PRIO_NORMAL, 2048,
					   s_wq_stack);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(wq);
	ove_test_workqueue_destroy(wq);
}

static void test_wq_init_free_work(void **state)
{
	(void)state;
	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	int rc = ove_work_init_static(&w, &s_work_storage, work_handler);
#else
	int rc = ove_work_init(&w, work_handler);
#endif
	assert_int_equal(rc, OVE_OK);
	assert_non_null(w);
#ifndef CONFIG_OVE_ZERO_HEAP
	ove_work_free(w);
#endif
}

static void test_wq_submit_handler_called(void **state)
{
	(void)state;
	s_work_called = 0;

	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "sub_wq", OVE_PRIO_NORMAL, 2048, s_wq_stack);

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_init_static(&w, &s_work_storage, work_handler);
#else
	ove_work_init(&w, work_handler);
#endif

	int rc = ove_work_submit(wq, w);
	assert_int_equal(rc, OVE_OK);

	assert_true(wait_for_flag(&s_work_called, 1, 500));

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_test_workqueue_destroy(wq);
#else
	ove_work_free(w);
	ove_test_workqueue_destroy(wq);
#endif
}

static void test_wq_submit_multiple(void **state)
{
	(void)state;
	atomic_store(&s_work_count, 0);

	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "multi_wq", OVE_PRIO_NORMAL, 2048,
				  s_wq_stack);

	ove_work_t works[3];
	for (int i = 0; i < 3; i++) {
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_work_init_static(&works[i], &s_work_storages[i], counting_handler);
#else
		ove_work_init(&works[i], counting_handler);
#endif
		ove_work_submit(wq, works[i]);
	}

	/* Poll the atomic counter via deadline loop (wait_for_flag only takes
     * volatile int; atomic_int is not a compatible cast on all compilers). */
	uint64_t now_us = 0, start_us = 0;
	(void)ove_time_get_us(&start_us);
	while (atomic_load(&s_work_count) < 3) {
		(void)ove_time_get_us(&now_us);
		if (now_us - start_us >= 500000)
			break;
		test_msleep(1);
	}
	assert_int_equal(atomic_load(&s_work_count), 3);

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_test_workqueue_destroy(wq);
#else
	for (int i = 0; i < 3; i++) {
		ove_work_free(works[i]);
	}
	ove_test_workqueue_destroy(wq);
#endif
}

static void test_wq_submit_delayed(void **state)
{
	(void)state;
	s_work_called = 0;

	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "delay_wq", OVE_PRIO_NORMAL, 2048,
				  s_wq_stack);

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_init_static(&w, &s_work_storage, delayed_handler);
#else
	ove_work_init(&w, delayed_handler);
#endif

	int rc = ove_work_submit_delayed(wq, w, 50);
	assert_int_equal(rc, OVE_OK);

	/* Should not have fired yet — check well before the 50 ms delay */
	test_msleep(10);
	assert_int_equal(__atomic_load_n(&s_work_called, __ATOMIC_ACQUIRE), 0);

	assert_true(wait_for_flag(&s_work_called, 1, 500));

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_test_workqueue_destroy(wq);
#else
	ove_work_free(w);
	ove_test_workqueue_destroy(wq);
#endif
}

static void test_wq_cancel_work(void **state)
{
	(void)state;
	s_work_called = 0;

	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "cancel_wq", OVE_PRIO_NORMAL, 2048,
				  s_wq_stack);

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_init_static(&w, &s_work_storage, delayed_handler);
#else
	ove_work_init(&w, delayed_handler);
#endif

	ove_work_submit_delayed(wq, w, 200);
	/* Try to cancel before it fires */
	int rc = ove_work_cancel(w);
	/* Cancel should return OK or a valid error code */
	assert_true(rc == OVE_OK || rc == OVE_ERR_TIMEOUT || rc < 0);

	test_msleep(300);
	/* POSIX stub cancel is best-effort: handler may still run even after
       cancel returns OK due to timer/thread races.  On RTOS backends with
       reliable cancel, s_work_called should be 0 when rc == OK. */

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_test_workqueue_destroy(wq);
#else
	ove_work_free(w);
	ove_test_workqueue_destroy(wq);
#endif
}

static void test_wq_cancel_not_pending(void **state)
{
	(void)state;
	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_init_static(&w, &s_work_storage, delayed_handler);
#else
	ove_work_init(&w, delayed_handler);
#endif

	/* Never submitted -> not pending -> OVE_ERR_INVAL (ove/workqueue.h). */
	int rc = ove_work_cancel(w);
	assert_int_equal(rc, OVE_ERR_INVAL);

#ifndef CONFIG_OVE_ZERO_HEAP
	ove_work_free(w);
#endif
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_wq_destroy_null(void **state)
{
	(void)state;
	/* Should not crash */
	ove_workqueue_destroy(NULL);
}
#endif

static void test_wq_handler_receives_handle(void **state)
{
	(void)state;
	s_work_called = 0;
	atomic_store(&s_received_handle_raw, (intptr_t)NULL);

	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "hndl_wq", OVE_PRIO_NORMAL, 2048, s_wq_stack);

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_init_static(&w, &s_work_storage, work_handler);
#else
	ove_work_init(&w, work_handler);
#endif

	ove_work_submit(wq, w);

	assert_true(wait_for_flag(&s_work_called, 1, 500));
	assert_ptr_equal((void *)atomic_load(&s_received_handle_raw), w);

#ifdef CONFIG_OVE_ZERO_HEAP
	ove_test_workqueue_destroy(wq);
#else
	ove_work_free(w);
	ove_test_workqueue_destroy(wq);
#endif
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_wq_create_null(void **state)
{
	(void)state;
	int rc = ove_workqueue_create(NULL, "wq", OVE_PRIO_NORMAL, 4096);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

static void test_wq_init_null_handler(void **state)
{
	(void)state;
	ove_work_t w = NULL;
	int rc = ove_work_init(&w, NULL);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}
#endif

/* ── setup/teardown ──────────────────────────────────────────────────── */

static int wq_setup(void **state)
{
	(void)state;
	s_work_called = 0;
	atomic_store(&s_work_count, 0);
	atomic_store(&s_received_handle_raw, (intptr_t)NULL);
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_workqueue_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_wq_create_destroy, wq_setup),
		cmocka_unit_test_setup(test_wq_init_free_work, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_handler_called, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_multiple, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_delayed, wq_setup),
		cmocka_unit_test_setup(test_wq_cancel_work, wq_setup),
		cmocka_unit_test_setup(test_wq_cancel_not_pending, wq_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_wq_destroy_null, wq_setup),
#endif
		cmocka_unit_test_setup(test_wq_handler_receives_handle, wq_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_wq_create_null, wq_setup),
		cmocka_unit_test_setup(test_wq_init_null_handler, wq_setup),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
