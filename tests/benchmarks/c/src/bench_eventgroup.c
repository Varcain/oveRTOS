/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"

static ove_eventgroup_t bench_eg;
static ove_eventgroup_storage_t bench_eg_storage;

/* --- set/get bits --- */

static void eg_set_get_setup(void *ctx)
{
	(void)ctx;
	ove_eventgroup_init(&bench_eg, &bench_eg_storage);
}

static void eg_set_get_run(void *ctx)
{
	(void)ctx;
	ove_eventgroup_set_bits(bench_eg, 0x01);
	ove_eventgroup_get_bits(bench_eg);
	ove_eventgroup_clear_bits(bench_eg, 0x01);
}

static void eg_set_get_teardown(void *ctx)
{
	(void)ctx;
	ove_eventgroup_deinit(bench_eg);
}

/* --- create/destroy + memory (heap-mode only) --- */
#ifndef CONFIG_OVE_ZERO_HEAP
static void eg_create_destroy_run(void *ctx)
{
	(void)ctx;
	ove_eventgroup_t eg;

	ove_eventgroup_create(&eg);
	ove_eventgroup_destroy(eg);
}

static ove_eventgroup_t mem_eg;

static void eg_memory_run(void *ctx)
{
	(void)ctx;
	ove_eventgroup_create(&mem_eg);
}

static void eg_memory_teardown(void *ctx)
{
	(void)ctx;
	ove_eventgroup_destroy(mem_eg);
}
#endif

/* --- Suite --- */

static int eventgroup_is_enabled(void)
{
	return 1;
}

static const bench_case_t eventgroup_cases[] = {
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "memory",
		.type = BENCH_TYPE_MEMORY,
		.run = eg_memory_run,
		.teardown = eg_memory_teardown,
	},
#endif
	{
		.name = "set_get_bits",
		.type = BENCH_TYPE_LATENCY,
		.setup = eg_set_get_setup,
		.run = eg_set_get_run,
		.teardown = eg_set_get_teardown,
	},
#ifndef CONFIG_OVE_ZERO_HEAP
	{
		.name = "create_destroy",
		.type = BENCH_TYPE_LATENCY,
		.run = eg_create_destroy_run,
	},
#endif
};

const bench_suite_t bench_suite_eventgroup = {
	.name = "eventgroup",
	.is_enabled = eventgroup_is_enabled,
	.cases = eventgroup_cases,
	.case_count = sizeof(eventgroup_cases) / sizeof(eventgroup_cases[0]),
};
