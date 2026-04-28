/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"
#include <stdio.h>

#define DIVIDER \
	"+-----------------------------+----------+-----------+-----------+-----------+------------+"

void bench_print_header(const char *suite_name)
{
	OVE_LOG("\n=== oveRTOS (%s) Benchmark: %s ===\n", OVE_RTOS_NAME, suite_name);
	OVE_LOG("%s\n", DIVIDER);
	OVE_LOG("| %-27s | %-8s | %9s | %9s | %9s | %10s |\n", "Case", "Type", "Min ns", "Avg ns",
		"Max ns", "Ops/s");
	OVE_LOG("%s\n", DIVIDER);
}

void bench_print_result(const bench_case_t *bc, const bench_result_t *result)
{
	if (bc->type == BENCH_TYPE_MEMORY) {
		if (result->heap_delta >= 0) {
			OVE_LOG("| %-27s | %-8s |"
				"                  %6d bytes"
				"                  |\n",
				bc->name, "MEMORY", (int)result->heap_delta);
		} else {
			OVE_LOG("| %-27s | %-8s |"
				"                      N/A"
				"                       |\n",
				bc->name, "MEMORY");
		}
		return;
	}

	const char *type_str = bc->type == BENCH_TYPE_LATENCY ? "LATENCY" : "THROUGH";
	uint64_t avg_ns = 0;

	if (result->count > 0)
		avg_ns = result->total_ns / result->count;

	OVE_LOG("| %-27s | %-8s | %9u | %9u | %9u | %10u |\n", bc->name, type_str,
		(unsigned int)result->min_ns, (unsigned int)avg_ns, (unsigned int)result->max_ns,
		(unsigned int)result->ops_per_sec);
}

void bench_print_footer(void)
{
	OVE_LOG("%s\n", DIVIDER);
}
