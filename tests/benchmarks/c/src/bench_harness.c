/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "benchmark.h"
#include "ove/ove.h"
#include <stdlib.h>
#include <string.h>

#if CONFIG_OVE_BENCHMARK_PERCENTILES
/*
 * Static sample buffer (BSS) — sized at CONFIG_OVE_BENCHMARK_ITERATIONS.
 * Cases that override `iterations` with a higher value get full
 * mean/min/max coverage but their percentiles are computed on the first
 * SAMPLE_BUFFER_SIZE samples only.  Default 1000 samples = 8 KiB.
 */
#define SAMPLE_BUFFER_SIZE CONFIG_OVE_BENCHMARK_ITERATIONS
static uint64_t sample_buffer[SAMPLE_BUFFER_SIZE];

static int u64_cmp(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

/*
 * Welford running variance — stable and avoids the catastrophic
 * cancellation a naive sum-of-squares would suffer at ns latencies
 * with µs-scale outliers.
 */
struct welford {
	uint64_t n;
	double mean; /* ns */
	double m2;
};

static void welford_push(struct welford *w, uint64_t sample)
{
	w->n++;
	double delta = (double)sample - w->mean;
	w->mean += delta / (double)w->n;
	double delta2 = (double)sample - w->mean;
	w->m2 += delta * delta2;
}

static uint64_t welford_stddev_q1000(const struct welford *w)
{
	if (w->n < 2)
		return 0;
	double var = w->m2 / (double)(w->n - 1);
	double sd = 0.0;
	if (var > 0.0) {
		/* Newton-Raphson sqrt — picolibc/newlib's sqrt() pulls in
		 * fenv on bare-metal targets and is overkill for a stddev
		 * report. ~6 iterations converges to sub-ppm accuracy from
		 * any positive seed.
		 */
		double x = var;
		for (int i = 0; i < 8; i++)
			x = 0.5 * (x + var / x);
		sd = x;
	}
	if (sd < 0.0)
		sd = 0.0;
	return (uint64_t)(sd * 1000.0 + 0.5);
}

static void compute_percentiles(uint64_t *samples, unsigned int n,
				bench_result_t *r)
{
	qsort(samples, n, sizeof(uint64_t), u64_cmp);

	r->p50_ns = samples[(n * 50) / 100];
	r->p95_ns = samples[(n * 95) / 100];
	if (n > 1)
		r->p99_ns = samples[((uint64_t)(n - 1) * 99) / 100];
	else
		r->p99_ns = samples[0];

	/* Trimmed mean: drop top 1% (rounded up to ≥1 sample on small
	 * counts) — robust against the occasional preempted iteration
	 * that would otherwise drag the arithmetic mean upward. */
	unsigned int trim = n / 100;
	if (trim == 0 && n > 10)
		trim = 1;
	unsigned int kept = n - trim;
	if (kept == 0)
		kept = n;
	uint64_t sum = 0;
	for (unsigned int i = 0; i < kept; i++)
		sum += samples[i];
	r->trimmed_mean_ns = sum / kept;
}
#endif /* CONFIG_OVE_BENCHMARK_PERCENTILES */

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

#if CONFIG_OVE_BENCHMARK_PERCENTILES
	struct welford w = { 0 };
	unsigned int sample_count = 0;
#endif

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

#if CONFIG_OVE_BENCHMARK_PERCENTILES
		welford_push(&w, elapsed);
		if (sample_count < SAMPLE_BUFFER_SIZE)
			sample_buffer[sample_count++] = elapsed;
#endif
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

#if CONFIG_OVE_BENCHMARK_PERCENTILES
	if (sample_count > 0) {
		compute_percentiles(sample_buffer, sample_count, result);
		result->stddev_ns_q = welford_stddev_q1000(&w);
	}
#endif
}
