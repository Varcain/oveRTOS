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
static volatile int s_work_finished;
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

static void running_handler(ove_work_t work)
{
	(void)work;
	TEST_FLAG_SET(s_work_called, 1);
	test_msleep(50);
	TEST_FLAG_SET(s_work_finished, 1);
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_wq_create_destroy(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "test_wq", OVE_PRIO_NORMAL,
						     2048, s_wq_stack));
	assert_non_null(wq);
	ove_test_workqueue_destroy(wq);
}

static void test_wq_init_free_work(void **state)
{
	(void)state;
	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, work_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, work_handler));
#endif
	assert_non_null(w);
	ove_test_work_destroy(w);
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

	OVE_TEST_ASSERT_OK(ove_work_submit(wq, w));

	assert_true(wait_for_flag(&s_work_called, 1, 500));

	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
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

	for (int i = 0; i < 3; i++) {
		ove_test_work_destroy(works[i]);
	}
	ove_test_workqueue_destroy(wq);
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

	OVE_TEST_ASSERT_OK(ove_work_submit_delayed(wq, w, 50));

	/* Should not have fired yet — check well before the 50 ms delay */
	test_msleep(10);
	assert_int_equal(__atomic_load_n(&s_work_called, __ATOMIC_ACQUIRE), 0);

	assert_true(wait_for_flag(&s_work_called, 1, 500));

	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
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

	/* Cancel immediately — well before the 200 ms delay could fire — so the
	 * outcome is unambiguous: cancel must synchronously drain the submission
	 * and the handler must never run. */
	ove_work_submit_delayed(wq, w, 200);
	OVE_TEST_ASSERT_OK(ove_work_cancel(w));

	/* Wait past the original delay window; a cancelled item never fires. */
	test_msleep(300);
	assert_int_equal(__atomic_load_n(&s_work_called, __ATOMIC_ACQUIRE), 0);

	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
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

	ove_test_work_destroy(w);
}

static void test_wq_resubmit_after_cancel(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "resub_wq", OVE_PRIO_NORMAL, 2048,
				  s_wq_stack);

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_work_init_static(&w, &s_work_storage, counting_handler);
#else
	ove_work_init(&w, counting_handler);
#endif

	/* Cancel the work, then submit it: the work MUST run.  NuttX sets a
	 * per-work 'cancelled' flag in ove_work_cancel() and previously left it
	 * set across a subsequent submit, so the resubmitted work was silently
	 * dropped — this regresses that. */
	atomic_store(&s_work_count, 0);
	(void)ove_work_cancel(w);
	ove_work_submit(wq, w);

	for (int i = 0; i < 200 && atomic_load(&s_work_count) == 0; i++)
		test_msleep(1);
	assert_int_equal(atomic_load(&s_work_count), 1);

	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
}

static void test_wq_rejects_duplicate_submission(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "busy_wq", OVE_PRIO_NORMAL,
						     2048, s_wq_stack));

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, delayed_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, delayed_handler));
#endif
	OVE_TEST_ASSERT_OK(ove_work_submit_delayed(wq, w, 200));
	assert_int_equal(ove_work_submit(wq, w), OVE_ERR_BUSY);
	OVE_TEST_ASSERT_OK(ove_work_cancel(w));
	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
}

static void test_wq_cancel_waits_for_running_handler(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "running_wq",
						     OVE_PRIO_NORMAL, 2048, s_wq_stack));

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, running_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, running_handler));
#endif
	OVE_TEST_ASSERT_OK(ove_work_submit(wq, w));
	assert_true(wait_for_flag(&s_work_called, 1, 500));
	assert_int_equal(ove_work_cancel(w), OVE_ERR_INVAL);
	assert_int_equal(__atomic_load_n(&s_work_finished, __ATOMIC_ACQUIRE), 1);
	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
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
	s_work_finished = 0;
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

	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
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
	s_work_finished = 0;
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
		cmocka_unit_test_setup(test_wq_resubmit_after_cancel, wq_setup),
		cmocka_unit_test_setup(test_wq_rejects_duplicate_submission, wq_setup),
		cmocka_unit_test_setup(test_wq_cancel_waits_for_running_handler, wq_setup),
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
