/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <ove/ove.hpp>
#include <ove/bench.hpp>

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

/* --- create/destroy --- */

static void eg_create_destroy_run()
{
	ove::EventGroup eg;
}

/* --- memory --- */

static std::optional<ove::EventGroup> mem_eg;

static void eg_memory_run()
{
	mem_eg.emplace();
}

static void eg_memory_teardown()
{
	mem_eg.reset();
}

/* --- Suite --- */

static bool eventgroup_is_enabled()
{
	return true;
}

static constexpr ove::bench::CaseSpec eg_memory_spec{
	.name = "memory",
	.kind = ove::bench::Type::memory,
	.run = &eg_memory_run,
	.teardown = &eg_memory_teardown,
};
static constexpr ove::bench::CaseSpec eg_set_get_spec{
	.name = "set_get_bits",
	.kind = ove::bench::Type::latency,
	.run = &eg_set_get_run,
	.setup = &eg_set_get_setup,
	.teardown = &eg_set_get_teardown,
};
static constexpr ove::bench::CaseSpec eg_create_destroy_spec{
	.name = "create_destroy",
	.kind = ove::bench::Type::latency,
	.run = &eg_create_destroy_run,
};

static constexpr bench_case_t eventgroup_cases[] = {
	ove::bench::case_<eg_memory_spec>(),
	ove::bench::case_<eg_set_get_spec>(),
	ove::bench::case_<eg_create_destroy_spec>(),
};

OVE_BENCH_SUITE(bench_suite_eventgroup, "eventgroup",
		eventgroup_is_enabled, eventgroup_cases)
