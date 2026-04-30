/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include "ove_bench.hpp"

#include <optional>

static std::optional<ove::Timer> bench_tmr;

static void timer_dummy_cb(ove_timer_t timer, void *user_data)
{
	(void)timer;
	(void)user_data;
}

/* --- create/destroy (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static void timer_create_destroy_run()
{
	ove::Timer t(timer_dummy_cb, nullptr, 1000, /*one_shot=*/false);
}
#endif

/* --- start/stop --- */

static void timer_start_stop_setup()
{
	bench_tmr.emplace(timer_dummy_cb, nullptr, 1000, /*one_shot=*/false);
}

static void timer_start_stop_run()
{
	(void)bench_tmr->start();
	(void)bench_tmr->stop();
}

static void timer_start_stop_teardown()
{
	bench_tmr.reset();
}

/* --- memory (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static std::optional<ove::Timer> mem_timer;

static void timer_memory_run()
{
	mem_timer.emplace(timer_dummy_cb, nullptr, 1000, /*one_shot=*/false);
}

static void timer_memory_teardown()
{
	mem_timer.reset();
}
#endif

/* --- Suite --- */

static bool timer_is_enabled()
{
	return true;
}

#ifndef CONFIG_OVE_ZERO_HEAP
static constexpr bench::CaseSpec timer_memory_spec{
	.name = "memory",
	.kind = bench::Type::memory,
	.run = &timer_memory_run,
	.teardown = &timer_memory_teardown,
};
static constexpr bench::CaseSpec timer_create_destroy_spec{
	.name = "create_destroy",
	.kind = bench::Type::latency,
	.run = &timer_create_destroy_run,
};
#endif
static constexpr bench::CaseSpec timer_start_stop_spec{
	.name = "start_stop",
	.kind = bench::Type::latency,
	.run = &timer_start_stop_run,
	.setup = &timer_start_stop_setup,
	.teardown = &timer_start_stop_teardown,
};

static constexpr bench_case_t timer_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	bench::case_<timer_memory_spec>(),
	bench::case_<timer_create_destroy_spec>(),
#endif
	bench::case_<timer_start_stop_spec>(),
};

OVE_BENCH_SUITE(bench_suite_timer, "timer", timer_is_enabled, timer_cases)
