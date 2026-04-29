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

/*
 * Tag identifying the binding under test.  Defined per-binding in the
 * build (see boards/host/posix/CMakeLists.txt for benchmark apps); the
 * default keeps human-readable output meaningful when no binding tag is
 * provided.  Used in the JSON envelope for cross-binding comparison.
 */
#ifndef OVE_APP_LANG_NAME
#define OVE_APP_LANG_NAME "unknown"
#endif

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

/* ─── JSON output (machine-readable, fed to scripts/bench_compare.py) ─
 *
 * Emits one JSON object per suite, matching the schema documented in
 * scripts/bench_compare.py.  Surrounded by sentinel banner lines so a
 * downstream parser can split it from the human ASCII tables in the
 * same stdout stream:
 *
 *   ###BENCH_JSON_BEGIN
 *   {"rtos":"posix","binding":"c","suite":"sync", ...}
 *   ###BENCH_JSON_END
 *
 * The ASCII path remains canonical for human use; JSON is only emitted
 * when CONFIG_OVE_BENCHMARK_OUTPUT_JSON=y so the default human flow is
 * unchanged.
 */
#if CONFIG_OVE_BENCHMARK_OUTPUT_JSON

void bench_emit_suite_json(const bench_suite_t *suite,
			   const bench_case_t *cases,
			   const bench_result_t *results,
			   unsigned int n)
{
	OVE_LOG("###BENCH_JSON_BEGIN\n");
	OVE_LOG("{\"rtos\":\"%s\",\"binding\":\"%s\",\"suite\":\"%s\",\"cases\":[",
		OVE_RTOS_NAME, OVE_APP_LANG_NAME, suite->name);

	for (unsigned int i = 0; i < n; i++) {
		const bench_case_t *bc = &cases[i];
		const bench_result_t *r = &results[i];
		const char *type_str =
			bc->type == BENCH_TYPE_LATENCY    ? "latency"
			: bc->type == BENCH_TYPE_THROUGHPUT ? "throughput"
							    : "memory";

		uint64_t avg_ns = (r->count > 0) ? r->total_ns / r->count : 0;

		OVE_LOG("%s{", i ? "," : "");
		OVE_LOG("\"name\":\"%s\",\"type\":\"%s\",", bc->name, type_str);

		if (bc->type == BENCH_TYPE_MEMORY) {
			OVE_LOG("\"heap_delta\":%d", (int)r->heap_delta);
		} else {
			OVE_LOG("\"min_ns\":%u,\"max_ns\":%u,\"avg_ns\":%u,",
				(unsigned int)r->min_ns,
				(unsigned int)r->max_ns,
				(unsigned int)avg_ns);
			OVE_LOG("\"count\":%u,\"ops_per_sec\":%u",
				(unsigned int)r->count,
				(unsigned int)r->ops_per_sec);
#if CONFIG_OVE_BENCHMARK_PERCENTILES
			OVE_LOG(",\"p50_ns\":%u,\"p95_ns\":%u,\"p99_ns\":%u,",
				(unsigned int)r->p50_ns,
				(unsigned int)r->p95_ns,
				(unsigned int)r->p99_ns);
			OVE_LOG("\"trimmed_mean_ns\":%u,\"stddev_ns_q1000\":%u",
				(unsigned int)r->trimmed_mean_ns,
				(unsigned int)r->stddev_ns_q);
#endif
		}
		OVE_LOG("}");
	}

	OVE_LOG("]}\n");
	OVE_LOG("###BENCH_JSON_END\n");
}

#endif /* CONFIG_OVE_BENCHMARK_OUTPUT_JSON */
