/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "ove/types.h"
#include "ove_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_OVE_BENCHMARK_ITERATIONS
#define CONFIG_OVE_BENCHMARK_ITERATIONS 1000
#endif

#ifndef CONFIG_OVE_BENCHMARK_WARMUP
#define CONFIG_OVE_BENCHMARK_WARMUP 100
#endif

/*
 * CONFIG_OVE_BENCHMARK_PERCENTILES enables per-iteration sample
 * collection so the harness can report p50/p95/p99 + a trimmed mean
 * (top 1% dropped — robust against scheduler jitter and the rare
 * preempted iteration that pulls min/max around).  Adds 8 KiB of RAM
 * (sample buffer at default ITERATIONS=1000) + qsort cost per case.
 * Default y where RAM is available; gate to n on the very smallest
 * targets if needed.
 */
#ifndef CONFIG_OVE_BENCHMARK_PERCENTILES
#define CONFIG_OVE_BENCHMARK_PERCENTILES 1
#endif

typedef enum {
	BENCH_TYPE_LATENCY,
	BENCH_TYPE_THROUGHPUT,
	BENCH_TYPE_MEMORY,
} bench_type_t;

typedef struct {
	const char *name;
	bench_type_t type;
	void (*setup)(void *ctx);
	void (*run)(void *ctx);
	void (*teardown)(void *ctx);
	unsigned int iterations; /* 0 = use default from Kconfig */
	/* For sub-µs ops, the timer-call overhead around bc->run dominates
	 * the per-iteration measurement.  Setting inner_iters > 1 has the
	 * harness call run() that many times between timestamp pairs and
	 * divide elapsed by inner_iters before recording, amortising the
	 * timer cost across multiple operations.  Default 0 = treat as 1. */
	unsigned int inner_iters;
} bench_case_t;

typedef struct {
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t total_ns;
	uint32_t count;
	uint32_t ops_per_sec;
	int32_t heap_delta; /* bytes, -1 if unsupported */
	/* Set when CONFIG_OVE_BENCHMARK_PERCENTILES, else 0. */
	uint64_t p50_ns;
	uint64_t p95_ns;
	uint64_t p99_ns;
	uint64_t trimmed_mean_ns; /* top 1% samples dropped */
	uint64_t stddev_ns_q;     /* fixed-point: stddev_ns × 1000 */
} bench_result_t;

typedef struct {
	const char *name;
	int (*is_enabled)(void);
	const bench_case_t *cases;
	unsigned int case_count;
} bench_suite_t;

/* Harness API */
void bench_run_case(const bench_case_t *bc, bench_result_t *result);

/* Output API */
void bench_print_header(const char *suite_name);
void bench_print_result(const bench_case_t *bc, const bench_result_t *result);
void bench_print_footer(void);

/* Memory API */
int32_t bench_get_free_heap(void);

/* Suite registrations */
extern const bench_suite_t bench_suite_time;
extern const bench_suite_t bench_suite_thread;
extern const bench_suite_t bench_suite_sync;
extern const bench_suite_t bench_suite_queue;
extern const bench_suite_t bench_suite_timer;
extern const bench_suite_t bench_suite_eventgroup;
extern const bench_suite_t bench_suite_workqueue;
extern const bench_suite_t bench_suite_stream;
extern const bench_suite_t bench_suite_native_posix;
extern const bench_suite_t bench_suite_native_freertos;
extern const bench_suite_t bench_suite_native_nuttx;

#if CONFIG_OVE_BENCHMARK_OUTPUT_JSON
void bench_emit_suite_json(const bench_suite_t *suite,
			   const bench_case_t *cases,
			   const bench_result_t *results,
			   unsigned int n);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BENCHMARK_H */
