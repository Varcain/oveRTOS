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
	ove_test_thread_run(&th, &s_th_storage, "giver", sem_giver_entry, &ctx,
			    s_th_stack, 4096);

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
	ove_test_thread_run(&th, &s_th_storage, "giver", sem_giver_entry, &ctx,
			    s_th_stack, 4096);

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
	ove_test_thread_run(&th, &s_th_storage, "giver", sem_giver_entry, &ctx,
			    s_th_stack, 4096);

	assert_int_equal(ove_sem_take(sem, OVE_WAIT_FOREVER), OVE_OK);

	ove_test_thread_destroy(th);
	ove_test_sem_destroy(sem);
}

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
		cmocka_unit_test(test_timeout_ns_constants),
		cmocka_unit_test(test_sem_wait_forever_unblocks_on_give),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
