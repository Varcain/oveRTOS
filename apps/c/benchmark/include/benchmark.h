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
} bench_case_t;

typedef struct {
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t total_ns;
	uint32_t count;
	uint32_t ops_per_sec;
	int32_t heap_delta; /* bytes, -1 if unsupported */
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

#ifdef __cplusplus
}
#endif

#endif /* BENCHMARK_H */
