/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "../framework/ove_test_isr.h"
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

static _Atomic uint32_t s_ran_ms; /* ove_time_get_us() / 1000 when it ran */

static void timestamp_handler(ove_work_t work)
{
	(void)work;
	uint64_t now_us = 0;
	(void)ove_time_get_us(&now_us);
	atomic_store(&s_ran_ms, (uint32_t)(now_us / 1000));
	TEST_FLAG_SET(s_work_finished, 1);
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

/* Contract the bindings rely on: the handle produced by ove_work_init_static
 * IS the caller's storage, so a binding can recover per-item context from the
 * handle its trampoline receives (e.g. by embedding the storage first in a
 * larger struct). */
static void test_wq_init_static_handle_is_storage(void **state)
{
	(void)state;
	ove_work_t w = NULL;
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, work_handler));
	assert_ptr_equal(w, (ove_work_t)&s_work_storage);
	ove_work_deinit(w);
}

static void test_wq_submit_handler_called(void **state)
{
	(void)state;
	s_work_called = 0;

	ove_workqueue_t wq = NULL;
	ove_test_workqueue_create(&wq, &s_wq_storage, "sub_wq", OVE_PRIO_NORMAL, 2048, s_wq_stack);

	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, work_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, work_handler));
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
		OVE_TEST_ASSERT_OK(
			ove_work_init_static(&works[i], &s_work_storages[i], counting_handler));
#else
		OVE_TEST_ASSERT_OK(ove_work_init(&works[i], counting_handler));
#endif
		OVE_TEST_ASSERT_OK(ove_work_submit(wq, works[i]));
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
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, delayed_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, delayed_handler));
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
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, delayed_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, delayed_handler));
#endif

	/* Cancel immediately — well before the 200 ms delay could fire — so the
	 * outcome is unambiguous: cancel must synchronously drain the submission
	 * and the handler must never run. */
	OVE_TEST_ASSERT_OK(ove_work_submit_delayed(wq, w, 200));
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
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, delayed_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, delayed_handler));
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
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, counting_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, counting_handler));
#endif

	/* Cancel the work, then submit it: the work MUST run.  NuttX sets a
	 * per-work 'cancelled' flag in ove_work_cancel() and previously left it
	 * set across a subsequent submit, so the resubmitted work was silently
	 * dropped — this regresses that. */
	atomic_store(&s_work_count, 0);
	(void)ove_work_cancel(w);
	OVE_TEST_ASSERT_OK(ove_work_submit(wq, w));

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

/* A submit refused because the item is already pending must not change it:
 * on NuttX it used to overwrite the item's delay, so a delayed item queued
 * behind a busy worker ran at once after an ove_work_submit() that returned
 * OVE_ERR_BUSY.  The delay counts from the submit on FreeRTOS and Zephyr and
 * from the worker taking the item on NuttX and the host; either way it has
 * passed by the time the item runs. */
static void test_wq_busy_resubmit_keeps_delay(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "busy_wq", OVE_PRIO_NORMAL,
						     2048, s_wq_stack));
	ove_work_t blocker = NULL;
	ove_work_t x = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&blocker, &s_work_storages[0], running_handler));
	OVE_TEST_ASSERT_OK(ove_work_init_static(&x, &s_work_storages[1], timestamp_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&blocker, running_handler));
	OVE_TEST_ASSERT_OK(ove_work_init(&x, timestamp_handler));
#endif
	/* The worker is busy for ~50 ms, so x waits in the queue. */
	OVE_TEST_ASSERT_OK(ove_work_submit(wq, blocker));
	assert_true(wait_for_flag(&s_work_called, 1, 500));

	uint64_t submit_us = 0;
	(void)ove_time_get_us(&submit_us);
	OVE_TEST_ASSERT_OK(ove_work_submit_delayed(wq, x, 300));
	assert_int_equal(ove_work_submit(wq, x), OVE_ERR_BUSY);

	assert_true(wait_for_flag(&s_work_finished, 1, 1000)); /* the blocker */
	TEST_FLAG_SET(s_work_finished, 0);
	assert_true(wait_for_flag(&s_work_finished, 1, 1500)); /* x */
	uint32_t waited_ms = atomic_load(&s_ran_ms) - (uint32_t)(submit_us / 1000);
	assert_true(waited_ms >= 290);

	ove_test_work_destroy(x);
	ove_test_work_destroy(blocker);
	ove_test_workqueue_destroy(wq);
}

/* Submitting from an interrupt: on FreeRTOS ove_work_submit asserted there,
 * on NuttX it could block the interrupted task.  The item runs on the
 * queue's thread; while it is still queued, a second submit is refused. */
struct isr_submit {
	ove_workqueue_t wq;
	ove_work_t work;
	int rc1;
	int rc2;
};

static void isr_submit_twice(void *arg)
{
	struct isr_submit *s = arg;
	s->rc1 = ove_work_submit_from_isr(s->wq, s->work);
	s->rc2 = ove_work_submit_from_isr(s->wq, s->work);
}

static void test_wq_submit_from_isr(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "isr_wq", OVE_PRIO_NORMAL,
						     2048, s_wq_stack));
	ove_work_t w = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, work_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, work_handler));
#endif
	struct isr_submit s = {.wq = wq, .work = w, .rc1 = -1, .rc2 = -1};
	bool in_isr = ove_test_run_in_isr(isr_submit_twice, &s);
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_true(in_isr);
	/* The worker cannot run while the interrupt does. */
	assert_int_equal(s.rc2, OVE_ERR_BUSY);
#else
	(void)in_isr;
#endif
	assert_int_equal(s.rc1, OVE_OK);
	assert_true(wait_for_flag(&s_work_called, 1, 500));
	assert_ptr_equal((void *)atomic_load(&s_received_handle_raw), (void *)w);

	ove_test_work_destroy(w);
	ove_test_workqueue_destroy(wq);
}

#if !defined(CONFIG_OVE_ZERO_HEAP) && !defined(CONFIG_OVE_RTOS_ZEPHYR)
/* With no room left, the interrupt-safe submit fails at once.  Zephyr's
 * queue is a list and cannot fill; the engines with a fixed queue hold 16
 * items (64 on the host). */
enum { FILL_MAX = 72 };

/* Holds the worker until the test sets s_work_finished to 2 (at most 3 s). */
static void gated_handler(ove_work_t work)
{
	(void)work;
	TEST_FLAG_SET(s_work_called, 1);
	for (int i = 0; i < 3000 && __atomic_load_n(&s_work_finished, __ATOMIC_ACQUIRE) != 2; i++)
		test_msleep(1);
}

static void isr_submit_once(void *arg)
{
	struct isr_submit *s = arg;
	s->rc1 = ove_work_submit_from_isr(s->wq, s->work);
}

static void test_wq_submit_from_isr_full_queue(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "full_wq", OVE_PRIO_NORMAL,
						     2048, s_wq_stack));
	ove_work_t blocker = NULL;
	OVE_TEST_ASSERT_OK(ove_work_init(&blocker, gated_handler));
	OVE_TEST_ASSERT_OK(ove_work_submit(wq, blocker));
	assert_true(wait_for_flag(&s_work_called, 1, 500)); /* busy until released */

	ove_work_t fill[FILL_MAX] = {0};
	int n = 0;
	int rc = OVE_OK;
	for (; n < FILL_MAX; n++) {
		OVE_TEST_ASSERT_OK(ove_work_init(&fill[n], counting_handler));
		rc = ove_work_submit_from_isr(wq, fill[n]);
		if (rc != OVE_OK)
			break;
	}
	assert_int_equal(rc, OVE_ERR_QUEUE_FULL);
	assert_true(n < FILL_MAX);

	struct isr_submit s = {.wq = wq, .work = fill[n], .rc1 = -1};
	bool in_isr = ove_test_run_in_isr(isr_submit_once, &s);
#ifndef CONFIG_OVE_RTOS_POSIX
	assert_true(in_isr);
#else
	(void)in_isr;
#endif
	assert_int_equal(s.rc1, OVE_ERR_QUEUE_FULL);
	assert_false(ove_work_is_pending(fill[n]));
	TEST_FLAG_SET(s_work_finished, 2); /* release the worker */

	/* Every queued item still runs once the worker is free. */
	uint64_t start_us = 0;
	uint64_t now_us = 0;
	(void)ove_time_get_us(&start_us);
	while (atomic_load(&s_work_count) < n) {
		test_msleep(5);
		(void)ove_time_get_us(&now_us);
		assert_true(now_us - start_us < 2000000);
	}
	for (int i = 0; i <= n; i++)
		ove_test_work_destroy(fill[i]);
	ove_test_work_destroy(blocker);
	ove_test_workqueue_destroy(wq);
}
#endif

/* Pending means queued or delayed and not yet started: the state
 * ove_work_cancel() removes an item from, and not the running one, which a
 * submit refuses with OVE_ERR_BUSY. */
static void test_wq_work_is_pending(void **state)
{
	(void)state;
	ove_workqueue_t wq = NULL;
	OVE_TEST_ASSERT_OK(ove_test_workqueue_create(&wq, &s_wq_storage, "pend_wq", OVE_PRIO_NORMAL,
						     2048, s_wq_stack));

	ove_work_t w = NULL;
	ove_work_t w2 = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, running_handler));
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w2, &s_work_storages[0], counting_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, running_handler));
	OVE_TEST_ASSERT_OK(ove_work_init(&w2, counting_handler));
#endif
	assert_false(ove_work_is_pending(w));

	OVE_TEST_ASSERT_OK(ove_work_submit_delayed(wq, w, 200));
	assert_true(ove_work_is_pending(w));
	OVE_TEST_ASSERT_OK(ove_work_cancel(w));
	assert_false(ove_work_is_pending(w));

	OVE_TEST_ASSERT_OK(ove_work_submit(wq, w));
	assert_true(wait_for_flag(&s_work_called, 1, 500));
	assert_false(ove_work_is_pending(w));
	assert_int_equal(ove_work_submit(wq, w), OVE_ERR_BUSY);

	/* Queued behind the running handler on the single worker. */
	OVE_TEST_ASSERT_OK(ove_work_submit(wq, w2));
	assert_true(ove_work_is_pending(w2));

	assert_true(wait_for_flag(&s_work_finished, 1, 500));
	for (int i = 0; i < 500 && atomic_load(&s_work_count) == 0; i++)
		test_msleep(1);
	assert_int_equal(atomic_load(&s_work_count), 1);
	assert_false(ove_work_is_pending(w2));
	assert_false(ove_work_is_pending(NULL));

	ove_test_work_destroy(w2);
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
	OVE_TEST_ASSERT_OK(ove_work_init_static(&w, &s_work_storage, work_handler));
#else
	OVE_TEST_ASSERT_OK(ove_work_init(&w, work_handler));
#endif

	OVE_TEST_ASSERT_OK(ove_work_submit(wq, w));

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
	atomic_store(&s_ran_ms, 0);
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

int test_workqueue_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_wq_create_destroy, wq_setup),
		cmocka_unit_test_setup(test_wq_init_free_work, wq_setup),
		cmocka_unit_test_setup(test_wq_init_static_handle_is_storage, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_handler_called, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_multiple, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_delayed, wq_setup),
		cmocka_unit_test_setup(test_wq_cancel_work, wq_setup),
		cmocka_unit_test_setup(test_wq_cancel_not_pending, wq_setup),
		cmocka_unit_test_setup(test_wq_resubmit_after_cancel, wq_setup),
		cmocka_unit_test_setup(test_wq_rejects_duplicate_submission, wq_setup),
		cmocka_unit_test_setup(test_wq_cancel_waits_for_running_handler, wq_setup),
		cmocka_unit_test_setup(test_wq_work_is_pending, wq_setup),
		cmocka_unit_test_setup(test_wq_busy_resubmit_keeps_delay, wq_setup),
		cmocka_unit_test_setup(test_wq_submit_from_isr, wq_setup),
#if !defined(CONFIG_OVE_ZERO_HEAP) && !defined(CONFIG_OVE_RTOS_ZEPHYR)
		cmocka_unit_test_setup(test_wq_submit_from_isr_full_queue, wq_setup),
#endif
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
