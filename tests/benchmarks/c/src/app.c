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
	&bench_suite_time,	   &bench_suite_thread,	    &bench_suite_sync,
	&bench_suite_queue,	   &bench_suite_timer,	    &bench_suite_eventgroup,
	&bench_suite_workqueue,	   &bench_suite_stream,	    &bench_suite_native_posix,
	&bench_suite_native_freertos,
	&bench_suite_native_nuttx,
};

#define SUITE_COUNT (sizeof(suites) / sizeof(suites[0]))

/* --- Runner thread --- */

/*
 * Max cases per suite — sized to fit any current suite (sync has the
 * largest case list at ~10 cases). Determines a static result-array
 * scratch buffer used to collect per-suite results before emitting
 * machine-readable JSON.  No heap allocation.
 */
#define MAX_CASES_PER_SUITE 32

static void benchmark_runner(void *arg)
{
	(void)arg;

	OVE_LOG_INF("=== oveRTOS Benchmark Suite ===");
	OVE_LOG_INF("Iterations: %d  Warmup: %d", CONFIG_OVE_BENCHMARK_ITERATIONS,
		    CONFIG_OVE_BENCHMARK_WARMUP);

	static bench_result_t results[MAX_CASES_PER_SUITE];

	for (unsigned int s = 0; s < SUITE_COUNT; s++) {
		const bench_suite_t *suite = suites[s];

		if (!suite->is_enabled()) {
			OVE_LOG_INF("Suite '%s': SKIPPED (module disabled)", suite->name);
			continue;
		}

		bench_print_header(suite->name);

		unsigned int n = suite->case_count;
		if (n > MAX_CASES_PER_SUITE)
			n = MAX_CASES_PER_SUITE;

		for (unsigned int c = 0; c < n; c++) {
			const bench_case_t *bc = &suite->cases[c];
			bench_run_case(bc, &results[c]);
			bench_print_result(bc, &results[c]);
		}

		bench_print_footer();

#if CONFIG_OVE_BENCHMARK_OUTPUT_JSON
		bench_emit_suite_json(suite, suite->cases, results, n);
#endif
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

	/*
	 * The benchmark measures dynamic create/destroy latency across
	 * its 8 suites — by definition every test case creates and
	 * destroys kernel resources (threads, workqueues, queues) at
	 * runtime, well after ove_main() has returned.
	 *
	 * On FreeRTOS and Zephyr in zero-heap mode this is purely
	 * static — the per-call-site `static` storage from the
	 * ove_thread_create / ove_workqueue_create macros is consumed by
	 * xTaskCreateStatic / k_thread_create, and no kernel allocation
	 * happens.  ove_run()'s auto-lock would harmlessly fire and
	 * trap nothing.
	 *
	 * On NuttX the kernel's task_create allocates from kmm on every
	 * thread create regardless of caller-supplied storage:
	 * sched/group/group_create.c:group_allocate() kmm_zallocs
	 * struct task_group_s for each TCB_FLAG_TTYPE_TASK.  That kmm
	 * activity inherently conflicts with ove_heap_lock, so the
	 * benchmark must bypass the auto-lock to run on NuttX.
	 *
	 * For consistency across backends, bypass on every RTOS — the
	 * scheduler kickoff is what we need; the lock is a nicety the
	 * benchmark does not benefit from.  See task #18 in the project
	 * tracker for the longer-term NuttX fix.
	 */
	ove_thread_start_scheduler();

	OVE_LOG_INF("Benchmark app: shutdown");
}
