/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Boundary-case coverage for the uint64_t timeout_ns ABI introduced
 * in Phase 3.  Exercises every blocking primitive at the values where
 * historical bugs hid: zero, sub-tick, the UINT32_MAX boundary where
 * the ns->tick fast path switches to the slow path, and the
 * OVE_WAIT_FOREVER sentinel.
 *
 * Timing notes: QEMU FreeRTOS / NuttX / Zephyr tick at 100–1000 Hz,
 * giving a 1–10 ms quantum on every timed wait.  All "elapsed must be
 * at least X" assertions use X >= 100 ms so a 1-tick rounding error
 * can't cause a false negative.  Upper bounds are omitted — they're
 * not the value, and they're the source of flake on slow QEMU.
 */

#include "../framework/ove_test.h"

OVE_TEST_STORAGE(ove_mutex_storage_t, s_mtx_storage);
OVE_TEST_STORAGE(ove_sem_storage_t, s_sem_storage);
OVE_TEST_STORAGE(ove_event_storage_t, s_evt_storage);
OVE_TEST_STORAGE(ove_queue_storage_t, s_q_storage);
static uint8_t s_q_buffer[sizeof(uint32_t) * 4];
OVE_TEST_STORAGE(ove_thread_storage_t, s_th_storage);
OVE_TEST_STACK(s_th_stack, 4096);

/* ── timeout = 0 (poll) ─────────────────────────────────────────────── */

static void test_sem_timeout_zero_returns_immediately(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	uint64_t before = ove_time_now_steady_ns();
	assert_int_equal(ove_sem_take(sem, 0), OVE_ERR_TIMEOUT);
	uint64_t elapsed = ove_time_now_steady_ns() - before;
	/* Must not block beyond a generous tick budget. */
	assert_true(elapsed < OVE_MS(50));
	ove_test_sem_destroy(sem);
}

static void test_event_timeout_zero_returns_immediately(void **state)
{
	(void)state;
	ove_event_t evt = NULL;
	ove_test_event_create(&evt, &s_evt_storage);
	assert_int_equal(ove_event_wait(evt, 0), OVE_ERR_TIMEOUT);
	ove_test_event_destroy(evt);
}

static void test_queue_send_zero_returns_full(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buffer, sizeof(uint32_t), 4);
	uint32_t item = 0xCAFEU;
	/* Fill the queue. */
	for (int i = 0; i < 4; i++)
		assert_int_equal(ove_queue_send(q, &item, 0), OVE_OK);
	/* P0-1: a full queue with timeout=0 returns QUEUE_FULL, not TIMEOUT. */
	assert_int_equal(ove_queue_send(q, &item, 0), OVE_ERR_QUEUE_FULL);
	ove_test_queue_destroy(q);
}

static void test_queue_receive_zero_returns_empty(void **state)
{
	(void)state;
	ove_queue_t q = NULL;
	ove_test_queue_create(&q, &s_q_storage, s_q_buffer, sizeof(uint32_t), 4);
	uint32_t item = 0;
	assert_int_equal(ove_queue_receive(q, &item, 0), OVE_ERR_QUEUE_EMPTY);
	ove_test_queue_destroy(q);
}

/* ── sub-tick timeout doesn't crash ─────────────────────────────────── */

static void test_sem_timeout_one_ns(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	/* 1 ns is below tick resolution on every backend; must time out
	 * (not crash, not block indefinitely). */
	assert_int_equal(ove_sem_take(sem, 1), OVE_ERR_TIMEOUT);
	ove_test_sem_destroy(sem);
}

/* ── realistic timed wait elapses for at least the requested duration ─ */

static void test_sem_timeout_hundred_ms_elapses(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);
	uint64_t before = ove_time_now_steady_ns();
	assert_int_equal(ove_sem_take(sem, OVE_MS(100)), OVE_ERR_TIMEOUT);
	uint64_t elapsed = ove_time_now_steady_ns() - before;
	/* Lower bound minus one tick of slop for clock-read quantization. */
	assert_true(elapsed >= OVE_MS(90));
	ove_test_sem_destroy(sem);
}

/* ── UINT32_MAX boundary (fast-path / slow-path divide) ─────────────── */

struct sem_giver_ctx {
	ove_sem_t sem;
	uint32_t after_ms;
};

static void sem_giver_entry(void *arg)
{
	struct sem_giver_ctx *ctx = arg;
	test_msleep(ctx->after_ms);
	ove_sem_give(ctx->sem);
}

static void test_sem_timeout_uint32_max_boundary(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);

	/* Spawn a worker that gives the sem promptly; the take itself
	 * uses a timeout exactly at UINT32_MAX (the largest value the
	 * ns->tick fast path handles) — must succeed without hanging. */
	struct sem_giver_ctx ctx = {.sem = sem, .after_ms = 30};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "giver", sem_giver_entry, &ctx, s_th_stack, 4096);

	assert_int_equal(ove_sem_take(sem, (uint64_t)UINT32_MAX), OVE_OK);

	ove_test_thread_destroy(th);
	ove_test_sem_destroy(sem);
}

static void test_sem_timeout_above_uint32_max_slow_path(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);

	/* UINT32_MAX + 1 forces the slow-path 64-bit divide; must succeed. */
	struct sem_giver_ctx ctx = {.sem = sem, .after_ms = 30};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "giver", sem_giver_entry, &ctx, s_th_stack, 4096);

	assert_int_equal(ove_sem_take(sem, (uint64_t)UINT32_MAX + 1ULL), OVE_OK);

	ove_test_thread_destroy(th);
	ove_test_sem_destroy(sem);
}

/* ── compile-time invariants ────────────────────────────────────────── */

static void test_timeout_ns_constants(void **state)
{
	(void)state;
	/* OVE_WAIT_FOREVER must be the new UINT64_MAX value, distinct
	 * from the old UINT32_MAX transitional value. */
	assert_true(OVE_WAIT_FOREVER == UINT64_MAX);
	assert_true(OVE_WAIT_FOREVER != (uint64_t)UINT32_MAX);
	/* Duration helpers compute the expected number of nanoseconds. */
	assert_true(OVE_NS(1) == 1ULL);
	assert_true(OVE_US(1) == 1000ULL);
	assert_true(OVE_MS(1) == 1000000ULL);
	assert_true(OVE_SEC(1) == 1000000000ULL);
}

/* ── OVE_WAIT_FOREVER unblocks on signal ─────────────────────────────── */

static void test_sem_wait_forever_unblocks_on_give(void **state)
{
	(void)state;
	ove_sem_t sem = NULL;
	ove_test_sem_create(&sem, &s_sem_storage, 0, 1);

	struct sem_giver_ctx ctx = {.sem = sem, .after_ms = 30};
	ove_thread_t th = NULL;
	ove_test_thread_run(&th, &s_th_storage, "giver", sem_giver_entry, &ctx, s_th_stack, 4096);

	assert_int_equal(ove_sem_take(sem, OVE_WAIT_FOREVER), OVE_OK);

	ove_test_thread_destroy(th);
	ove_test_sem_destroy(sem);
}

#if defined(CONFIG_OVE_RTOS_NUTTX) || defined(CONFIG_OVE_RTOS_FREERTOS)
OVE_TEST_STORAGE(ove_mutex_storage_t, s_range_recursive);
OVE_TEST_STORAGE(ove_condvar_storage_t, s_range_condvar);
OVE_TEST_STORAGE(ove_sem_storage_t, s_range_ready);
OVE_TEST_STORAGE(ove_eventgroup_storage_t, s_range_group);
OVE_TEST_STORAGE(ove_stream_storage_t, s_range_stream);
static unsigned char s_range_bytes[4];

struct range_wake_context {
	unsigned kind;
	ove_mutex_t mutex, recursive;
	ove_sem_t sem, ready;
	ove_event_t event;
	ove_queue_t queue;
	ove_condvar_t condvar;
	ove_eventgroup_t group;
	ove_stream_t stream;
	int status;
};

static void range_wake_entry(void *arg)
{
	struct range_wake_context *c = arg;
	if (c->kind < 2) {
		c->status = c->kind ? ove_recursive_mutex_lock(c->recursive, OVE_MS(500))
				    : ove_mutex_lock(c->mutex, OVE_MS(500));
		ove_sem_give(c->ready);
		test_msleep(30);
		if (c->status == OVE_OK) {
			if (c->kind)
				ove_recursive_mutex_unlock(c->recursive);
			else
				ove_mutex_unlock(c->mutex);
		}
		return;
	}
	test_msleep(15);
	uint32_t item = 123;
	size_t count = 0;
	switch (c->kind) {
	case 2:
		ove_sem_give(c->sem);
		break;
	case 3:
		ove_event_signal(c->event);
		break;
	case 4:
		c->status = ove_queue_send(c->queue, &item, 0);
		break;
	case 5:
		(void)ove_eventgroup_set_bits(c->group, 2);
		test_msleep(15);
		(void)ove_eventgroup_set_bits(c->group, 1);
		break;
	case 6:
		c->status = ove_stream_send(c->stream, &item, 1, 0, &count);
		if (count != 1)
			c->status = OVE_ERR_BUSY;
		test_msleep(15);
		if (c->status == OVE_OK) {
			c->status = ove_stream_send(c->stream, &item, 1, 0, &count);
			if (count != 1)
				c->status = OVE_ERR_BUSY;
		}
		break;
	case 7:
		c->status = ove_mutex_lock(c->mutex, OVE_MS(500));
		if (c->status == OVE_OK) {
			ove_condvar_signal(c->condvar);
			ove_mutex_unlock(c->mutex);
		}
		break;
	}
}

#ifdef CONFIG_OVE_RTOS_NUTTX
static void test_nuttx_large_finite_prompt_release(void **state)
#else
static void test_freertos_large_finite_prompt_release(void **state)
#endif
{
	(void)state;
	struct range_wake_context c = {0};
	assert_int_equal(ove_test_mutex_create(&c.mutex, &s_mtx_storage), OVE_OK);
	assert_int_equal(ove_test_recursive_mutex_create(&c.recursive, &s_range_recursive), OVE_OK);
	assert_int_equal(ove_test_sem_create(&c.sem, &s_sem_storage, 0, 1), OVE_OK);
	assert_int_equal(ove_test_sem_create(&c.ready, &s_range_ready, 0, 1), OVE_OK);
	assert_int_equal(ove_test_event_create(&c.event, &s_evt_storage), OVE_OK);
	assert_int_equal(ove_test_queue_create(&c.queue, &s_q_storage, s_q_buffer, sizeof(uint32_t),
					       4),
			 OVE_OK);
	assert_int_equal(ove_test_condvar_create(&c.condvar, &s_range_condvar), OVE_OK);
	assert_int_equal(ove_test_eventgroup_create(&c.group, &s_range_group), OVE_OK);
	assert_int_equal(ove_test_stream_create(&c.stream, &s_range_stream, s_range_bytes,
						sizeof(s_range_bytes), 2),
			 OVE_OK);
	for (c.kind = 0; c.kind < 8; ++c.kind) {
#ifdef CONFIG_OVE_RTOS_FREERTOS
		/* Binary Event time accounting has its own independent repair. */
		if (c.kind == 3)
			continue;
#endif
		c.status = OVE_OK;
		if (c.kind == 7)
			assert_int_equal(ove_mutex_lock(c.mutex, OVE_MS(500)), OVE_OK);
		ove_thread_t worker = NULL;
		assert_int_equal(ove_test_thread_run(&worker, &s_th_storage, "range-wake",
						     range_wake_entry, &c, s_th_stack,
						     sizeof(s_th_stack)),
				 OVE_OK);
		if (c.kind < 2)
			assert_int_equal(ove_sem_take(c.ready, OVE_SEC(1)), OVE_OK);
		int rc = OVE_ERR_BUSY;
		uint32_t item = 0;
		size_t count = 0;
		ove_eventbits_t bits = 0;
		bool held = true;
		switch (c.kind) {
		case 0:
			rc = ove_mutex_lock(c.mutex, UINT64_MAX - 1);
			if (rc == OVE_OK)
				ove_mutex_unlock(c.mutex);
			break;
		case 1:
			rc = ove_recursive_mutex_lock(c.recursive, UINT64_MAX - 1);
			if (rc == OVE_OK)
				ove_recursive_mutex_unlock(c.recursive);
			break;
		case 2:
			rc = ove_sem_take(c.sem, UINT64_MAX - 1);
			break;
		case 3:
			rc = ove_event_wait(c.event, UINT64_MAX - 1);
			break;
		case 4:
			rc = ove_queue_receive(c.queue, &item, UINT64_MAX - 1);
			break;
		case 5:
			rc = ove_eventgroup_wait_bits(c.group, 1, 0, UINT64_MAX - 1, &bits);
			break;
		case 6:
			rc = ove_stream_receive(c.stream, &item, 2, UINT64_MAX - 1, &count);
			break;
		case 7:
			rc = ove_condvar_wait_checked(c.condvar, c.mutex, UINT64_MAX - 1, &held);
			if (held)
				ove_mutex_unlock(c.mutex);
			break;
		}
		assert_int_equal(ove_test_thread_join_destroy(worker, 2000), OVE_OK);
		assert_int_equal(rc, OVE_OK);
		assert_int_equal(c.status, OVE_OK);
		if (c.kind == 4)
			assert_int_equal(item, 123);
		if (c.kind == 5)
			assert_int_equal(bits, 3);
		if (c.kind == 6)
			assert_int_equal(count, 2);
		if (c.kind == 7)
			assert_true(held);
	}
	ove_test_stream_destroy(c.stream);
	ove_test_eventgroup_destroy(c.group);
	ove_test_condvar_destroy(c.condvar);
	ove_test_queue_destroy(c.queue);
	ove_test_event_destroy(c.event);
	ove_test_sem_destroy(c.ready);
	ove_test_sem_destroy(c.sem);
	ove_test_recursive_mutex_destroy(c.recursive);
	ove_test_mutex_destroy(c.mutex);
}
#endif

#ifdef CONFIG_OVE_RTOS_FREERTOS
struct queue_drain_context {
	ove_queue_t queue;
	uint32_t value;
	int status;
};
static void queue_drain_entry(void *arg)
{
	struct queue_drain_context *ctx = arg;
	test_msleep(30);
	ctx->status = ove_queue_receive(ctx->queue, &ctx->value, 0);
}
static void test_freertos_large_finite_queue_send(void **state)
{
	(void)state;
	ove_queue_t queue = NULL;
	assert_int_equal(ove_test_queue_create(&queue, &s_q_storage, s_q_buffer, sizeof(uint32_t),
					       4),
			 OVE_OK);
	uint32_t value = 123;
	for (unsigned i = 0; i < 4; ++i)
		assert_int_equal(ove_queue_send(queue, &value, 0), OVE_OK);
	struct queue_drain_context ctx = {.queue = queue, .status = OVE_ERR_BUSY};
	ove_thread_t worker = NULL;
	assert_int_equal(ove_test_thread_run(&worker, &s_th_storage, "range-drain",
					     queue_drain_entry, &ctx, s_th_stack,
					     sizeof(s_th_stack)),
			 OVE_OK);
	int result = ove_queue_send(queue, &value, UINT64_MAX - 1);
	assert_int_equal(ove_test_thread_join_destroy(worker, 2000), OVE_OK);
	assert_int_equal(result, OVE_OK);
	assert_int_equal(ctx.status, OVE_OK);
	assert_int_equal(ctx.value, value);
	assert_int_equal(ove_queue_count(queue), 4);
	for (unsigned i = 0; i < 4; ++i)
		assert_int_equal(ove_queue_receive(queue, &value, 0), OVE_OK);
	ove_test_queue_destroy(queue);
}
#endif

/* ── runner ─────────────────────────────────────────────────────────── */

int test_timeout_ns_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_sem_timeout_zero_returns_immediately),
		cmocka_unit_test(test_event_timeout_zero_returns_immediately),
		cmocka_unit_test(test_queue_send_zero_returns_full),
		cmocka_unit_test(test_queue_receive_zero_returns_empty),
		cmocka_unit_test(test_sem_timeout_one_ns),
		cmocka_unit_test(test_sem_timeout_hundred_ms_elapses),
		cmocka_unit_test(test_sem_timeout_uint32_max_boundary),
		cmocka_unit_test(test_sem_timeout_above_uint32_max_slow_path),
#ifdef CONFIG_OVE_RTOS_NUTTX
		cmocka_unit_test(test_nuttx_large_finite_prompt_release),
#endif
#ifdef CONFIG_OVE_RTOS_FREERTOS
		cmocka_unit_test(test_freertos_large_finite_prompt_release),
		cmocka_unit_test(test_freertos_large_finite_queue_send),
#endif
		cmocka_unit_test(test_timeout_ns_constants),
		cmocka_unit_test(test_sem_wait_forever_unblocks_on_give),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
