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
	while (ove_queue_receive(q, &val, 200) == OVE_OK) {
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
		s_blocking_done = 1;
	}
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_queue_create_destroy(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	int rc = ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);
	assert_int_equal(rc, OVE_OK);
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
		ove_queue_send(q, &i, 0);
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
	int rc = ove_queue_send(q, &v, 10);
	assert_int_equal(rc, OVE_ERR_TIMEOUT);

	ove_test_queue_destroy(q);
}

static void test_queue_receive_empty_times_out(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int val;
	int rc = ove_queue_receive(q, &val, 10);
	assert_int_equal(rc, OVE_ERR_TIMEOUT);

	ove_test_queue_destroy(q);
}

static void test_queue_send_from_isr(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	int v = 99;
	ove_queue_send_from_isr(q, &v);

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
	ove_queue_send(q, &v, 0);

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
	ove_test_thread_run(&th, &s_th_storage, "consumer", consumer_thread, q, s_th_stack, 4096);

	/* produce 1+2+3+4+5 = 15 */
	for (int i = 1; i <= 5; i++) {
		ove_queue_send(q, &i, 100);
		test_msleep(5);
	}

	/* let consumer drain and timeout-exit */
	test_msleep(500);
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
#endif

static void test_queue_send_wait_forever(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buf, sizeof(int), 5);

	atomic_store(&s_blocking_received, 0);
	s_blocking_done = 0;

	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "blocker", blocking_receiver, q, s_th_stack, 4096);

	test_msleep(50); /* let receiver block — no observable flag before it enters receive() */

	int v = 123;
	ove_queue_send(q, &v, 0);

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
	int rc = ove_test_queue_create(&q, &s_q_storage_pair, s_q_buf_pair, sizeof(struct {
		int a;
		int b;
	}),
				       8);
	assert_int_equal(rc, OVE_OK);
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

#ifndef CONFIG_OVE_ZERO_HEAP
static void test_queue_create_null_handle(void **state)
{
	(void)state;
	int rc = ove_queue_create(NULL, sizeof(int), 5);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}
#endif

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

int test_queue_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_queue_create_destroy, queue_setup),
		cmocka_unit_test_setup(test_queue_send_receive_single, queue_setup),
		cmocka_unit_test_setup(test_queue_fifo_order, queue_setup),
		cmocka_unit_test_setup(test_queue_send_full_times_out, queue_setup),
		cmocka_unit_test_setup(test_queue_receive_empty_times_out, queue_setup),
		cmocka_unit_test_setup(test_queue_send_from_isr, queue_setup),
		cmocka_unit_test_setup(test_queue_receive_from_isr, queue_setup),
		cmocka_unit_test_setup(test_queue_producer_consumer, queue_setup),
		cmocka_unit_test_setup(test_queue_struct_item, queue_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_queue_destroy_null, queue_setup),
#endif
		cmocka_unit_test_setup(test_queue_send_wait_forever, queue_setup),
		cmocka_unit_test_setup(test_queue_pair_item_size, queue_setup),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test_setup(test_queue_create_null_handle, queue_setup),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
