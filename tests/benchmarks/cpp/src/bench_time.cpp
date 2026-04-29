/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include "ove_bench.hpp"

/* --- time_get_us overhead --- */

static void time_get_us_overhead_run()
{
	uint64_t t;
	(void)ove::time::get_us(&t);
}

/* --- delay 1ms --- */

static void delay_1ms_run()
{
	ove::time::delay_ms(1);
}

/* --- Suite --- */

static bool time_is_enabled()
{
	return true;
}

static constexpr bench::CaseSpec time_get_us_spec{
	.name = "time_get_us_overhead",
	.kind = bench::Type::latency,
	.run = &time_get_us_overhead_run,
	.inner_iters = 10,
};

static constexpr bench::CaseSpec delay_1ms_spec{
	.name = "delay_1ms",
	.kind = bench::Type::latency,
	.run = &delay_1ms_run,
	.iterations = 100,
};

static constexpr bench_case_t time_cases[] = {
	bench::case_<time_get_us_spec>(),
	bench::case_<delay_1ms_spec>(),
};

OVE_BENCH_SUITE(bench_suite_time, "time", time_is_enabled, time_cases)
