/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include <stdatomic.h>

OVE_TEST_STORAGE(ove_queue_storage_t, s_q_storage);
static uint8_t s_q_buf[sizeof(int) * 10];
OVE_TEST_STORAGE(ove_queue_storage_t, s_q_storage_pair);
static uint8_t s_q_buf_pair[sizeof(int) * 2 * 8];
OVE_TEST_STORAGE(ove_queue_storage_t, s_q_null_storage);
static uint8_t s_q_null_buf[sizeof(int) * 4];
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

/* ── helpers ─────────────────────────────────────────────────────────── */

typedef struct {
	int a;
	int b;
} pair_t;

static atomic_int s_consumer_sum;

static void consumer_thread(void *arg)
{
	ove_queue_t q = (ove_queue_t)arg;
	int val;
	while (ove_queue_receive(q, &val, OVE_MS(200)) == OVE_OK) {
		atomic_fetch_add(&s_consumer_sum, val);
	}
}

static volatile int s_blocking_done;
static atomic_int s_blocking_received;

static void blocking_receiver(void *arg)
{
	ove_queue_t q = (ove_queue_t)arg;
	int val;
	if (ove_queue_receive(q, &val, OVE_WAIT_FOREVER) == OVE_OK) {
		atomic_store(&s_blocking_received, val);
		TEST_FLAG_SET(s_blocking_done, 1);
	}
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_queue_create_destroy(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	OVE_TEST_ASSERT_OK(ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5));
	assert_non_null(q);
	ove_test_queue_destroy(q);
}

static void test_queue_send_receive_single(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int send_val = 42;
	int rc = ove_queue_send(q, &send_val, 0);
	assert_int_equal(rc, OVE_OK);

	int recv_val = 0;
	rc = ove_queue_receive(q, &recv_val, 0);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(recv_val, 42);

	ove_test_queue_destroy(q);
}

static void test_queue_fifo_order(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 10);

	for (int i = 0; i < 5; i++) {
		assert_int_equal(ove_queue_send(q, &i, 0), OVE_OK);
	}

	for (int i = 0; i < 5; i++) {
		int val = -1;
		int rc = ove_queue_receive(q, &val, 0);
		assert_int_equal(rc, OVE_OK);
		assert_int_equal(val, i);
	}

	ove_test_queue_destroy(q);
}

static void test_queue_send_full_times_out(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 2);

	int v = 1;
	assert_int_equal(ove_queue_send(q, &v, 0), OVE_OK);
	v = 2;
	assert_int_equal(ove_queue_send(q, &v, 0), OVE_OK);
	v = 3;
	int rc = ove_queue_send(q, &v, OVE_MS(10));
	assert_int_equal(rc, OVE_ERR_TIMEOUT);

	ove_test_queue_destroy(q);
}

/* P0-1: distinguishes "queue full, no wait" from "waited and timed out".
 * Header contracts OVE_ERR_QUEUE_FULL for timeout_ms==0 + full queue. */
static void test_queue_send_full_zero_timeout_returns_full(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 2);

	int v = 1;
	assert_int_equal(ove_queue_send(q, &v, 0), OVE_OK);
	v = 2;
	assert_int_equal(ove_queue_send(q, &v, 0), OVE_OK);
	v = 3;
	int rc = ove_queue_send(q, &v, 0);
	assert_int_equal(rc, OVE_ERR_QUEUE_FULL);

	ove_test_queue_destroy(q);
}

static void test_queue_receive_empty_times_out(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int val;
	int rc = ove_queue_receive(q, &val, OVE_MS(10));
	assert_int_equal(rc, OVE_ERR_TIMEOUT);

	ove_test_queue_destroy(q);
}

/* P0-2: distinguishes "queue empty, no wait" from "waited and timed out". */
static void test_queue_receive_empty_zero_timeout_returns_empty(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int val;
	int rc = ove_queue_receive(q, &val, 0);
	assert_int_equal(rc, OVE_ERR_QUEUE_EMPTY);

	ove_test_queue_destroy(q);
}

/* P0-2: ISR-variant empty case must report QUEUE_EMPTY (was TIMEOUT). */
static void test_queue_receive_from_isr_empty_returns_empty(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int val;
	int rc = ove_queue_receive_from_isr(q, &val);
	assert_int_equal(rc, OVE_ERR_QUEUE_EMPTY);

	ove_test_queue_destroy(q);
}

static void test_queue_send_from_isr(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int v = 99;
	assert_int_equal(ove_queue_send_from_isr(q, &v), OVE_OK);

	int out = 0;
	int rc = ove_queue_receive(q, &out, 0);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(out, 99);

	ove_test_queue_destroy(q);
}

static void test_queue_receive_from_isr(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int v = 77;
	assert_int_equal(ove_queue_send(q, &v, 0), OVE_OK);

	int out = 0;
	int rc = ove_queue_receive_from_isr(q, &out);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(out, 77);

	ove_test_queue_destroy(q);
}

static void test_queue_producer_consumer(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 10);

	atomic_store(&s_consumer_sum, 0);

	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "consumer", consumer_thread, q,
					       s_th_stack, 4096));

	/* produce 1+2+3+4+5 = 15 */
	for (int i = 1; i <= 5; i++) {
		assert_int_equal(ove_queue_send(q, &i, OVE_MS(100)), OVE_OK);
		test_msleep(5);
	}

	/* ove_test_thread_destroy joins: it blocks until consumer_thread returns,
	 * which happens once it has drained the queue and its next receive times
	 * out — so the sum is final without a fixed settle delay. */
	ove_test_thread_destroy(th);

	assert_int_equal(atomic_load(&s_consumer_sum), 15);
	ove_test_queue_destroy(q);
}

static void test_queue_struct_item(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage_pair, s_q_buf_pair, sizeof(pair_t), 4);

	pair_t p = {.a = 10, .b = 20};
	assert_int_equal(ove_queue_send(q, &p, 0), OVE_OK);

	pair_t out = {0};
	assert_int_equal(ove_queue_receive(q, &out, 0), OVE_OK);
	assert_int_equal(out.a, 10);
	assert_int_equal(out.b, 20);

	ove_test_queue_destroy(q);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_queue_destroy_null(void **state)
{
	(void)state;
	/* Should not crash */
	ove_queue_destroy(NULL);
}

static void test_queue_create_overflow_rejected(void **state)
{
	(void)state;
	/* item_size * max_items overflows size_t — must be rejected, not wrapped
	 * to an undersized buffer (which would heap-overflow on send). */
	ove_queue_t q = NULL;
	int rc = ove_queue_create(&q, SIZE_MAX / 2 + 1, 2);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_null(q);
}
#endif

static void test_queue_send_wait_forever(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	atomic_store(&s_blocking_received, 0);
	s_blocking_done = 0;

	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "blocker", blocking_receiver, q,
					       s_th_stack, 4096));

	test_msleep(50); /* let receiver block — no observable flag before it enters receive() */

	int v = 123;
	assert_int_equal(ove_queue_send(q, &v, 0), OVE_OK);

	assert_true(wait_for_flag(&s_blocking_done, 1, 500));
	assert_int_equal(atomic_load(&s_blocking_received), 123);

	ove_test_thread_destroy(th);
	ove_test_queue_destroy(q);
}

static void test_queue_pair_item_size(void **state)
{
	(void)state;
	/* Verify we can create a queue with item_size = sizeof(pair_t) = 2*sizeof(int) */
	ove_queue_t q = NULL;
	OVE_TEST_ASSERT_OK(ove_test_queue_create(&q, &s_q_storage_pair, s_q_buf_pair,
						 sizeof(struct {
							 int a;
							 int b;
						 }),
						 8));
	assert_non_null(q);

	struct {
		int a;
		int b;
	} item = {.a = 100, .b = 200};
	assert_int_equal(ove_queue_send(q, &item, 0), OVE_OK);

	struct {
		int a;
		int b;
	} out = {0};
	assert_int_equal(ove_queue_receive(q, &out, 0), OVE_OK);
	assert_int_equal(out.a, 100);
	assert_int_equal(out.b, 200);

	ove_test_queue_destroy(q);
}

/* ── NULL-parameter rejection ────────────────────────────────────────── */

/* See test_mutex_init_null_out_rejected: the callee's own check must hold. */
static void test_queue_init_null_out_rejected(void **state)
{
	(void)state;
	ove_queue_t *volatile out = NULL;
	assert_int_equal(ove_queue_init(out, &s_q_null_storage, s_q_null_buf, sizeof(int), 4),
			 OVE_ERR_INVALID_PARAM);
}

static void test_queue_init_null_buffer_rejected(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	void *volatile buf = NULL;
	assert_int_equal(ove_queue_init(&q, &s_q_null_storage, buf, sizeof(int), 4),
			 OVE_ERR_INVALID_PARAM);
}

/* ── setup/teardown ──────────────────────────────────────────────────── */

static int queue_setup(void **state)
{
	(void)state;
	atomic_store(&s_consumer_sum, 0);
	atomic_store(&s_blocking_received, 0);
	s_blocking_done = 0;
	return 0;
}

/* ── runner ──────────────────────────────────────────────────────────── */

static void test_queue_init_null_storage_rejected(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_queue_storage_t *volatile storage = NULL;
	assert_int_equal(ove_queue_init(&q, storage, s_q_null_buf, sizeof(int), 4),
			 OVE_ERR_INVALID_PARAM);
}

static void test_queue_set_notify_null_rejected(void **state)
{
	(void)state;
	ove_queue_t volatile q = NULL;
	assert_int_equal(ove_queue_set_notify(q, NULL, NULL), OVE_ERR_INVALID_PARAM);
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_queue_create_null_out_rejected(void **state)
{
	(void)state;
	ove_queue_t *volatile out = NULL;
	assert_int_equal(ove_queue_create(out, sizeof(int), 4), OVE_ERR_INVALID_PARAM);
}
#endif

static void test_queue_count_and_capacity(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	OVE_TEST_ASSERT_OK(ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 4));

	assert_int_equal(ove_queue_count(q), 0);
	assert_int_equal(ove_queue_count_from_isr(q), 0);
	assert_int_equal(ove_queue_capacity(q), 4);

	int v = 0;
	for (int i = 0; i < 3; i++)
		OVE_TEST_ASSERT_OK(ove_queue_send(q, &i, 0));
	assert_int_equal(ove_queue_count(q), 3);
	assert_int_equal(ove_queue_count_from_isr(q), 3);

	OVE_TEST_ASSERT_OK(ove_queue_receive(q, &v, 0));
	assert_int_equal(ove_queue_count(q), 2);

	OVE_TEST_ASSERT_OK(ove_queue_send(q, &v, 0));
	OVE_TEST_ASSERT_OK(ove_queue_send(q, &v, 0));
	assert_int_equal(ove_queue_count(q), 4);
	assert_int_equal(ove_queue_capacity(q), 4);

	for (int i = 0; i < 4; i++)
		OVE_TEST_ASSERT_OK(ove_queue_receive(q, &v, 0));
	assert_int_equal(ove_queue_count(q), 0);
	ove_test_queue_destroy(q);

	assert_int_equal(ove_queue_count(NULL), 0);
	assert_int_equal(ove_queue_count_from_isr(NULL), 0);
	assert_int_equal(ove_queue_capacity(NULL), 0);
}

struct parked_rx {
	ove_queue_t q;
	volatile int parked;
	volatile int done;
};

static void parked_receiver(void *arg)
{
	struct parked_rx *p = arg;
	int v = 0;
	TEST_FLAG_SET(p->parked, 1);
	int rc = ove_queue_receive(p->q, &v, OVE_WAIT_FOREVER);
	TEST_FLAG_SET(p->done, rc == OVE_OK ? 1 : -1);
}

/* A receiver blocked on an empty queue must not show up as a count (NuttX
 * counts items with a semaphore that goes negative for each waiter). */
static void test_queue_count_with_waiting_receiver(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	OVE_TEST_ASSERT_OK(ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 4));

	struct parked_rx p = {.q = q};
	ove_thread_t th = NULL;
	OVE_TEST_ASSERT_OK(ove_test_thread_run(&th, &s_th_storage, "qcnt_rx", parked_receiver, &p,
					       s_th_stack, 4096));
	assert_true(wait_for_flag(&p.parked, 1, 500));
	test_msleep(50);
	size_t waiting = ove_queue_count(q);
	size_t waiting_isr = ove_queue_count_from_isr(q);

	int v = 42;
	OVE_TEST_ASSERT_OK(ove_queue_send(q, &v, 0));
	int done = wait_for_flag(&p.done, 1, 1000);
	OVE_TEST_ASSERT_OK(ove_test_thread_join_destroy(th, 1000));
	ove_test_queue_destroy(q);

	assert_int_equal(waiting, 0);
	assert_int_equal(waiting_isr, 0);
	assert_true(done);
}

int test_queue_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_queue_create_destroy, queue_setup),
		cmocka_unit_test_setup(test_queue_send_receive_single, queue_setup),
		cmocka_unit_test_setup(test_queue_fifo_order, queue_setup),
		cmocka_unit_test_setup(test_queue_send_full_times_out, queue_setup),
		cmocka_unit_test_setup(test_queue_send_full_zero_timeout_returns_full, queue_setup),
		cmocka_unit_test_setup(test_queue_receive_empty_times_out, queue_setup),
		cmocka_unit_test_setup(test_queue_receive_empty_zero_timeout_returns_empty,
				       queue_setup),
		cmocka_unit_test_setup(test_queue_receive_from_isr_empty_returns_empty,
				       queue_setup),
		cmocka_unit_test_setup(test_queue_send_from_isr, queue_setup),
		cmocka_unit_test_setup(test_queue_receive_from_isr, queue_setup),
		cmocka_unit_test_setup(test_queue_producer_consumer, queue_setup),
		cmocka_unit_test_setup(test_queue_struct_item, queue_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_queue_destroy_null, queue_setup),
		cmocka_unit_test_setup(test_queue_create_overflow_rejected, queue_setup),
#endif
		cmocka_unit_test_setup(test_queue_send_wait_forever, queue_setup),
		cmocka_unit_test_setup(test_queue_pair_item_size, queue_setup),
		cmocka_unit_test_setup(test_queue_init_null_out_rejected, queue_setup),
		cmocka_unit_test_setup(test_queue_init_null_buffer_rejected, queue_setup),
		cmocka_unit_test(test_queue_init_null_storage_rejected),
		cmocka_unit_test(test_queue_set_notify_null_rejected),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_queue_create_null_out_rejected),
#endif
		cmocka_unit_test(test_queue_count_and_capacity),
		cmocka_unit_test(test_queue_count_with_waiting_receiver),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
