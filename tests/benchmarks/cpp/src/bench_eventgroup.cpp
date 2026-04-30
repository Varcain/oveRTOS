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

static std::optional<ove::EventGroup> bench_eg;

/* --- set/get bits --- */

static void eg_set_get_setup()
{
	bench_eg.emplace();
}

static void eg_set_get_run()
{
	(void)bench_eg->set_bits(0x01);
	(void)bench_eg->get_bits();
	(void)bench_eg->clear_bits(0x01);
}

static void eg_set_get_teardown()
{
	bench_eg.reset();
}

/* --- create/destroy + memory (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static void eg_create_destroy_run()
{
	ove::EventGroup eg;
}

static std::optional<ove::EventGroup> mem_eg;

static void eg_memory_run()
{
	mem_eg.emplace();
}

static void eg_memory_teardown()
{
	mem_eg.reset();
}
#endif

/* --- Suite --- */

static bool eventgroup_is_enabled()
{
	return true;
}

#ifndef CONFIG_OVE_ZERO_HEAP
static constexpr bench::CaseSpec eg_memory_spec{
	.name = "memory",
	.kind = bench::Type::memory,
	.run = &eg_memory_run,
	.teardown = &eg_memory_teardown,
};
#endif
static constexpr bench::CaseSpec eg_set_get_spec{
	.name = "set_get_bits",
	.kind = bench::Type::latency,
	.run = &eg_set_get_run,
	.setup = &eg_set_get_setup,
	.teardown = &eg_set_get_teardown,
};
#ifndef CONFIG_OVE_ZERO_HEAP
static constexpr bench::CaseSpec eg_create_destroy_spec{
	.name = "create_destroy",
	.kind = bench::Type::latency,
	.run = &eg_create_destroy_run,
};
#endif

static constexpr bench_case_t eventgroup_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	bench::case_<eg_memory_spec>(),
#endif
	bench::case_<eg_set_get_spec>(),
#ifndef CONFIG_OVE_ZERO_HEAP
	bench::case_<eg_create_destroy_spec>(),
#endif
};

OVE_BENCH_SUITE(bench_suite_eventgroup, "eventgroup", eventgroup_is_enabled, eventgroup_cases)
