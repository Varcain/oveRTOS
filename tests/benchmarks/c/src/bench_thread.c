/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"

/* --- Context for thread benchmarks ---
 *
 * Setup paths use _init() with file-scope storage so the bench compiles
 * in both heap and zero-heap modes.  The create/destroy throughput
 * case is heap-only — it measures allocation cost, which has no
 * zero-heap analog. */

static ove_thread_t bench_th;
static ove_thread_storage_t bench_th_storage;
OVE_THREAD_STACK_DEFINE_STATIC_(bench_th_stack, 2048);
static ove_sem_t ping_sem;
static ove_sem_storage_t ping_sem_storage;
static ove_sem_t pong_sem;
static ove_sem_storage_t pong_sem_storage;
static volatile int ctx_switch_done;

/* --- create/destroy (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static void dummy_thread(void *arg)
{
	(void)arg;
}

static void thread_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_thread_t th;
	ove_thread_create(&th, "bench_tmp", dummy_thread, NULL, OVE_PRIO_LOW, 1024);
	ove_thread_destroy(th);
}
#endif

/* --- yield --- */

static void thread_yield_run(void *ctx)
{
	(void)ctx;
	ove_thread_yield();
}

/* --- sleep 1ms --- */

static void thread_sleep_1ms_run(void *ctx)
{
	(void)ctx;
	ove_thread_sleep_ms(1);
}

/* --- context switch via semaphore ping-pong --- */

static void pong_thread(void *arg)
{
	(void)arg;

	while (!ctx_switch_done) {
		ove_sem_take(ping_sem, OVE_WAIT_FOREVER);
		ove_sem_give(pong_sem);
	}
}

static void ctx_switch_setup(void *ctx)
{
	(void)ctx;
	ctx_switch_done = 0;
	ove_sem_init(&ping_sem, &ping_sem_storage, 0, 1);
	ove_sem_init(&pong_sem, &pong_sem_storage, 0, 1);
	ove_thread_init(&bench_th, &bench_th_storage, "pong", pong_thread, NULL, OVE_PRIO_NORMAL,
			sizeof(bench_th_stack), bench_th_stack);
}

static void ctx_switch_run(void *ctx)
{
	(void)ctx;
	/* One round-trip = 2 context switches */
	ove_sem_give(ping_sem);
	ove_sem_take(pong_sem, OVE_WAIT_FOREVER);
}

static void ctx_switch_teardown(void *ctx)
{
	(void)ctx;
	ctx_switch_done = 1;
	ove_sem_give(ping_sem);
	ove_thread_sleep_ms(10);
	ove_thread_deinit(bench_th);
	ove_sem_deinit(ping_sem);
	ove_sem_deinit(pong_sem);
}

static int thread_is_enabled(void)
{
	return 1;
}

static const bench_case_t thread_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = thread_create_destroy_run,
		.iterations = 200,
	},
#endif
	{
		.name = "yield",
		.type = BENCH_TYPE_LATENCY,
		.run = thread_yield_run,
	},
	{
		.name = "sleep_1ms",
		.type = BENCH_TYPE_LATENCY,
		.run = thread_sleep_1ms_run,
		.iterations = 100,
	},
	{
		.name = "context_switch",
		.type = BENCH_TYPE_LATENCY,
		.setup = ctx_switch_setup,
		.run = ctx_switch_run,
		.teardown = ctx_switch_teardown,
		.iterations = 500,
	},
};

const bench_suite_t bench_suite_thread = {
	.name = "thread",
	.is_enabled = thread_is_enabled,
	.cases = thread_cases,
	.case_count = sizeof(thread_cases) / sizeof(thread_cases[0]),
};
