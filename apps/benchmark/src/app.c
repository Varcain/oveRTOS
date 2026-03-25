/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS Benchmark Application
 *
 * Measures latency, throughput, and memory usage of all RTOS abstractions.
 * Output is formatted ASCII tables via OVE_LOG.
 */

#include "benchmark.h"
#include "ove/ove.h"

/* --- Suite registry --- */

static const bench_suite_t *const suites[] = {
	&bench_suite_time,
	&bench_suite_thread,
	&bench_suite_sync,
	&bench_suite_queue,
	&bench_suite_timer,
	&bench_suite_eventgroup,
	&bench_suite_workqueue,
	&bench_suite_stream,
};

#define SUITE_COUNT (sizeof(suites) / sizeof(suites[0]))

/* --- Runner thread --- */

static void benchmark_runner(void *arg)
{
	(void)arg;

	OVE_LOG_INF("=== oveRTOS Benchmark Suite ===");
	OVE_LOG_INF("Iterations: %d  Warmup: %d",
			CONFIG_OVE_BENCHMARK_ITERATIONS,
			CONFIG_OVE_BENCHMARK_WARMUP);

	for (unsigned int s = 0; s < SUITE_COUNT; s++) {
		const bench_suite_t *suite = suites[s];

		if (!suite->is_enabled()) {
			OVE_LOG_INF("Suite '%s': SKIPPED (module disabled)",
					suite->name);
			continue;
		}

		bench_print_header(suite->name);

		for (unsigned int c = 0; c < suite->case_count; c++) {
			const bench_case_t *bc = &suite->cases[c];
			bench_result_t result;

			bench_run_case(bc, &result);
			bench_print_result(bc, &result);
		}

		bench_print_footer();
	}

	OVE_LOG_INF("=== Benchmark complete ===");
}

/* --- App entry point --- */

void ove_main(void)
{
	OVE_LOG_INF("Benchmark app: init");

	ove_thread_t handle;
	static const struct ove_thread_desc bench_desc = {
		.name = "bench_run",
		.entry = benchmark_runner,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
	};

	int ret = ove_thread_create(&handle, 8192, &bench_desc);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to create benchmark thread: %d", ret);
		return;
	}

	ove_run();

	OVE_LOG_INF("Benchmark app: shutdown");
}
