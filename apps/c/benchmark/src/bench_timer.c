/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"

static ove_timer_t bench_tmr;

static void timer_dummy_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
}

/* --- create/destroy --- */

static void timer_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_timer_t t;

	ove_timer_create(&t, timer_dummy_cb, NULL, 1000, 0);
	ove_timer_destroy(t);
}

/* --- start/stop --- */

static void timer_start_stop_setup(void *ctx)
{
	(void)ctx;
	ove_timer_create(&bench_tmr, timer_dummy_cb, NULL, 1000, 0);
}

static void timer_start_stop_run(void *ctx)
{
	(void)ctx;
	ove_timer_start(bench_tmr);
	ove_timer_stop(bench_tmr);
}

static void timer_start_stop_teardown(void *ctx)
{
	(void)ctx;
	ove_timer_destroy(bench_tmr);
}

/* --- memory --- */

static ove_timer_t mem_timer;

static void timer_memory_run(void *ctx)
{
	(void)ctx;
	ove_timer_create(&mem_timer, timer_dummy_cb, NULL, 1000, 0);
}

static void timer_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_timer_destroy(mem_timer);
}

/* --- Suite --- */

static int timer_is_enabled(void)
{
#ifdef CONFIG_OVE_TIMER
	return 1;
#else
	return 0;
#endif
}

static const bench_case_t timer_cases[] = {
	{
		.name = "memory",
		.type = BENCH_TYPE_MEMORY,
		.run = timer_memory_run,
		.teardown = timer_memory_teardown,
	},
	{
		.name = "create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = timer_create_destroy_run,
	},
	{
		.name = "start_stop",
		.type = BENCH_TYPE_LATENCY,
		.setup = timer_start_stop_setup,
		.run = timer_start_stop_run,
		.teardown = timer_start_stop_teardown,
	},
};

const bench_suite_t bench_suite_timer = {
	.name = "timer",
	.is_enabled = timer_is_enabled,
	.cases = timer_cases,
	.case_count = sizeof(timer_cases) / sizeof(timer_cases[0]),
};
