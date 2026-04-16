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

	ove_timer_create(&t, timer_dummy_cb, nullptr, 1000, 0);
	ove_timer_destroy(t);
}

/* --- start/stop --- */

static void timer_start_stop_setup(void *ctx)
{
	(void)ctx;
	ove_timer_create(&bench_tmr, timer_dummy_cb, nullptr, 1000, 0);
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
	ove_timer_create(&mem_timer, timer_dummy_cb, nullptr, 1000, 0);
}

static void timer_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_timer_destroy(mem_timer);
}

/* --- Suite --- */

static int timer_is_enabled(void)
{
	return 1;
}

static const bench_case_t timer_cases[] = {
	{
		"memory",
		BENCH_TYPE_MEMORY,
		nullptr,
		timer_memory_run,
		timer_memory_teardown,
		0,
	},
	{
		"create_destroy",
		BENCH_TYPE_LATENCY,
		nullptr,
		timer_create_destroy_run,
		nullptr,
		0,
	},
	{
		"start_stop",
		BENCH_TYPE_LATENCY,
		timer_start_stop_setup,
		timer_start_stop_run,
		timer_start_stop_teardown,
		0,
	},
};

extern "C" const bench_suite_t bench_suite_timer = {
	"timer",
	timer_is_enabled,
	timer_cases,
	sizeof(timer_cases) / sizeof(timer_cases[0]),
};
