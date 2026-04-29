/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"

static void time_get_us_overhead_run(void *ctx)
{
	(void)ctx;
	uint64_t t;

	ove_time_get_us(&t);
}

static void delay_1ms_run(void *ctx)
{
	(void)ctx;
	ove_time_delay_ms(1);
}

static int time_is_enabled(void)
{
	return 1;
}

static const bench_case_t time_cases[] = {
	{
		.name = "time_get_us_overhead",
		.type = BENCH_TYPE_LATENCY,
		.run = time_get_us_overhead_run,
		/* Sub-µs op — amortise the harness's per-iter ove_time_get_ns
		 * call overhead by running ×10 inner per timestamp pair. */
		.inner_iters = 10,
	},
	{
		.name = "delay_1ms",
		.type = BENCH_TYPE_LATENCY,
		.run = delay_1ms_run,
		.iterations = 100,
	},
};

const bench_suite_t bench_suite_time = {
	.name = "time",
	.is_enabled = time_is_enabled,
	.cases = time_cases,
	.case_count = sizeof(time_cases) / sizeof(time_cases[0]),
};
