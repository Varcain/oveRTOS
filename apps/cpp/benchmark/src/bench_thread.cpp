/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>

extern "C" {
#include "benchmark.h"
}

/* --- Context for thread benchmarks --- */

static ove_thread_t bench_th;
static ove_sem_t ping_sem;
static ove_sem_t pong_sem;
static volatile int ctx_switch_done;

/* --- create/destroy --- */

static void dummy_thread(void *arg)
{
	(void)arg;
}

static void thread_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_thread_t th;
	struct ove_thread_desc desc = {};
	desc.name = "bench_tmp";
	desc.entry = dummy_thread;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_LOW;

	ove_thread_create(&th, 1024, &desc);
	ove_thread_destroy(th);
}

/* --- yield --- */

static void thread_yield_run(void *ctx)
{
	(void)ctx;
	ove::Thread<>::yield();
}

/* --- sleep 1ms --- */

static void thread_sleep_1ms_run(void *ctx)
{
	(void)ctx;
	ove::Thread<>::sleep_ms(1);
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
	ove_sem_create(&ping_sem, 0, 1);
	ove_sem_create(&pong_sem, 0, 1);

	struct ove_thread_desc desc = {};
	desc.name = "pong";
	desc.entry = pong_thread;
	desc.arg = nullptr;
	desc.priority = OVE_PRIO_NORMAL;

	ove_thread_create(&bench_th, 2048, &desc);
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
	ove_thread_destroy(bench_th);
	ove_sem_destroy(ping_sem);
	ove_sem_destroy(pong_sem);
}

/* --- Suite --- */

static int thread_is_enabled(void)
{
	return 1;
}

static const bench_case_t thread_cases[] = {
	{
		"create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		thread_create_destroy_run,
		nullptr,
		200,
	},
	{
		"yield",
		BENCH_TYPE_LATENCY,
		nullptr,
		thread_yield_run,
		nullptr,
		0,
	},
	{
		"sleep_1ms",
		BENCH_TYPE_LATENCY,
		nullptr,
		thread_sleep_1ms_run,
		nullptr,
		100,
	},
	{
		"context_switch",
		BENCH_TYPE_LATENCY,
		ctx_switch_setup,
		ctx_switch_run,
		ctx_switch_teardown,
		500,
	},
};

extern "C" const bench_suite_t bench_suite_thread = {
	"thread",
	thread_is_enabled,
	thread_cases,
	sizeof(thread_cases) / sizeof(thread_cases[0]),
};
