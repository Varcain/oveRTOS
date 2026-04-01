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

/* --- time_get_us overhead --- */

static void time_get_us_overhead_run(void *ctx)
{
	(void)ctx;
	uint64_t t;

	ove::time::get_us(&t);
}

/* --- delay 1ms --- */

static void delay_1ms_run(void *ctx)
{
	(void)ctx;
	ove::time::delay_ms(1);
}

/* --- Suite --- */

static int time_is_enabled(void)
{
#ifdef CONFIG_OVE_TIME
	return 1;
#else
	return 0;
#endif
}

static const bench_case_t time_cases[] = {
	{
		"time_get_us_overhead",
		BENCH_TYPE_LATENCY,
		nullptr,
		time_get_us_overhead_run,
		nullptr,
		0,
	},
	{
		"delay_1ms",
		BENCH_TYPE_LATENCY,
		nullptr,
		delay_1ms_run,
		nullptr,
		100,
	},
};

extern "C" const bench_suite_t bench_suite_time = {
	"time",
	time_is_enabled,
	time_cases,
	sizeof(time_cases) / sizeof(time_cases[0]),
};
