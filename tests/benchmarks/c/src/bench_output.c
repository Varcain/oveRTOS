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

/* Per-case JSON gets composed into a single stack buffer and emitted via
 * one OVE_LOG call.  Splitting it across many small OVE_LOG calls (the
 * old shape) tickles a race on Zephyr's deferred logger and on NuttX's
 * line-buffered console where back-to-back writes can interleave or
 * resurface stale buffer content as duplicate JSON keys.  See
 * `grep -lE '"trimmed_mean_ns":[0-9]+.*"trimmed_mean_ns"' output/...`
 * for the symptom.  768 bytes covers the longest case (~280 chars with
 * percentiles + stddev, +6 audit checkpoints when audit mode is on). */
static int json_case_format(char *buf, size_t cap, const bench_case_t *bc, const bench_result_t *r)
{
	const char *type_str = bc->type == BENCH_TYPE_LATENCY	   ? "latency"
			       : bc->type == BENCH_TYPE_THROUGHPUT ? "throughput"
								   : "memory";

	if (bc->type == BENCH_TYPE_MEMORY) {
		return snprintf(buf, cap, "{\"name\":\"%s\",\"type\":\"%s\",\"heap_delta\":%d}",
				bc->name, type_str, (int)r->heap_delta);
	}

	uint64_t avg_ns = (r->count > 0) ? r->total_ns / r->count : 0;
	int n;

#if CONFIG_OVE_BENCHMARK_PERCENTILES
	n = snprintf(buf, cap,
		     "{\"name\":\"%s\",\"type\":\"%s\","
		     "\"min_ns\":%u,\"max_ns\":%u,\"avg_ns\":%u,"
		     "\"count\":%u,\"ops_per_sec\":%u,"
		     "\"p50_ns\":%u,\"p95_ns\":%u,\"p99_ns\":%u,"
		     "\"trimmed_mean_ns\":%u,\"stddev_ns_q1000\":%u",
		     bc->name, type_str, (unsigned int)r->min_ns, (unsigned int)r->max_ns,
		     (unsigned int)avg_ns, (unsigned int)r->count, (unsigned int)r->ops_per_sec,
		     (unsigned int)r->p50_ns, (unsigned int)r->p95_ns, (unsigned int)r->p99_ns,
		     (unsigned int)r->trimmed_mean_ns, (unsigned int)r->stddev_ns_q);
#else
	n = snprintf(buf, cap,
		     "{\"name\":\"%s\",\"type\":\"%s\","
		     "\"min_ns\":%u,\"max_ns\":%u,\"avg_ns\":%u,"
		     "\"count\":%u,\"ops_per_sec\":%u",
		     bc->name, type_str, (unsigned int)r->min_ns, (unsigned int)r->max_ns,
		     (unsigned int)avg_ns, (unsigned int)r->count, (unsigned int)r->ops_per_sec);
#endif
	if (n <= 0 || (size_t)n >= cap)
		return n;

	/* audit_count is always 0 unless CONFIG_OVE_BENCHMARK_NOISE_AUDIT
	 * is on (the snapshot site is gated; the struct field always
	 * exists so the layout matches across bindings). */
	if (r->audit_count > 0) {
		int m = snprintf(buf + n, cap - (size_t)n, ",\"audit\":[");
		if (m <= 0 || (size_t)(n + m) >= cap)
			return n + m;
		n += m;
		for (uint8_t i = 0; i < r->audit_count; i++) {
			m = snprintf(buf + n, cap - (size_t)n,
				     "%s{\"n\":%u,\"mean_ns\":%u,\"stddev_ns_q1000\":%u}",
				     i ? "," : "", (unsigned int)r->audit_points[i].n,
				     (unsigned int)r->audit_points[i].mean_ns,
				     (unsigned int)r->audit_points[i].stddev_ns_q);
			if (m <= 0 || (size_t)(n + m) >= cap)
				return n + m;
			n += m;
		}
		m = snprintf(buf + n, cap - (size_t)n, "]");
		if (m <= 0 || (size_t)(n + m) >= cap)
			return n + m;
		n += m;
	}

	int m = snprintf(buf + n, cap - (size_t)n, "}");
	if (m <= 0)
		return m;
	return n + m;
}

/* Each per-case JSON is composed in a single 512-byte stack buffer and
 * pushed atomically to ove_console_write — bypassing the 256-byte
 * `OVE_LOG`/`_OVE_LOG_RAW` buffer that would truncate longer cases. */
void bench_emit_suite_json(const bench_suite_t *suite, const bench_case_t *cases,
			   const bench_result_t *results, unsigned int n)
{
	OVE_LOG("###BENCH_JSON_BEGIN\n");
	OVE_LOG("{\"rtos\":\"%s\",\"binding\":\"%s\",\"suite\":\"%s\",\"cases\":[", OVE_RTOS_NAME,
		OVE_APP_LANG_NAME, suite->name);

	char json_buf[768];
	int emitted = 0;
	for (unsigned int i = 0; i < n; i++) {
		int payload_len =
			json_case_format(json_buf, sizeof(json_buf), &cases[i], &results[i]);
		/* Skipping a case (format failure or buffer overflow) must NOT
		 * leave a leading comma on the next case — track successful
		 * emits, not the loop index, so a skipped case 0 doesn't
		 * produce "[,{...}" malformed JSON. */
		if (payload_len <= 0 || (size_t)payload_len >= sizeof(json_buf))
			continue;
		if (emitted > 0)
			ove_console_write(",", 1);
		ove_console_write(json_buf, (unsigned int)payload_len);
		emitted++;
	}

	OVE_LOG("]}\n");
	OVE_LOG("###BENCH_JSON_END\n");
}

#endif /* CONFIG_OVE_BENCHMARK_OUTPUT_JSON */
