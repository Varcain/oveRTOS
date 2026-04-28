/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"
#include <string.h>

void bench_run_case(const bench_case_t *bc, bench_result_t *result)
{
	unsigned int iters = bc->iterations;
	unsigned int warmup = CONFIG_OVE_BENCHMARK_WARMUP;

	if (iters == 0)
		iters = CONFIG_OVE_BENCHMARK_ITERATIONS;

	memset(result, 0, sizeof(*result));
	result->min_ns = UINT64_MAX;
	result->heap_delta = -1;

	if (bc->type == BENCH_TYPE_MEMORY) {
		int32_t best = -1;
		int attempts = 3;

		if (bc->setup)
			bc->setup(NULL);

		for (int i = 0; i < attempts; i++) {
			int32_t before = bench_get_free_heap();
			bc->run(NULL);
			int32_t after = bench_get_free_heap();

			if (bc->teardown)
				bc->teardown(NULL);

			if (before >= 0 && after >= 0) {
				int32_t delta = before - after;
				if (delta >= 0 && (best < 0 || delta < best))
					best = delta;
			}
		}

		result->heap_delta = best;
		result->count = 1;
		return;
	}

	if (bc->setup)
		bc->setup(NULL);

	/* Warmup */
	for (unsigned int i = 0; i < warmup; i++)
		bc->run(NULL);

	/* Measurement */
	for (unsigned int i = 0; i < iters; i++) {
		uint64_t start = 0, end = 0;

		ove_time_get_ns(&start);
		bc->run(NULL);
		ove_time_get_ns(&end);

		uint64_t elapsed = end - start;

		if (elapsed < result->min_ns)
			result->min_ns = elapsed;
		if (elapsed > result->max_ns)
			result->max_ns = elapsed;
		result->total_ns += elapsed;
		result->count++;
	}

	if (bc->teardown)
		bc->teardown(NULL);

	if (bc->type == BENCH_TYPE_THROUGHPUT && result->total_ns > 0) {
		result->ops_per_sec =
			(uint32_t)((uint64_t)result->count * 1000000000ULL / result->total_ns);
	} else if (bc->type == BENCH_TYPE_LATENCY && result->total_ns > 0) {
		result->ops_per_sec =
			(uint32_t)((uint64_t)result->count * 1000000000ULL / result->total_ns);
	}
}
