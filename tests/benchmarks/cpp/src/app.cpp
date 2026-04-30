/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Benchmark Application
 *
 * Measures latency, throughput, and memory usage of all RTOS abstractions
 * through the safe C++ binding layer. Output is formatted ASCII tables
 * via OVE_LOG (shared C harness).
 */

#include <ove/ove.hpp>
#include "ove_bench.hpp"

/* --- Suite registry --- */

static const bench_suite_t *const suites[] = {
	&bench_suite_time,	   &bench_suite_thread,
	&bench_suite_sync,	   &bench_suite_queue,
	&bench_suite_timer,	   &bench_suite_eventgroup,
	&bench_suite_workqueue,	   &bench_suite_stream,
	&bench_suite_native_posix, &bench_suite_native_freertos,
	&bench_suite_native_nuttx, &bench_suite_native_zephyr,
};

static constexpr unsigned int SUITE_COUNT = sizeof(suites) / sizeof(suites[0]);

/* --- Runner thread --- */

static void benchmark_runner(void *arg)
{
	(void)arg;

	OVE_LOG_INF("=== oveRTOS C++ Benchmark Suite ===");
	OVE_LOG_INF("Iterations: %d  Warmup: %d", CONFIG_OVE_BENCHMARK_ITERATIONS,
		    CONFIG_OVE_BENCHMARK_WARMUP);

	for (unsigned int s = 0; s < SUITE_COUNT; s++) {
		bench::run_suite(*suites[s]);
	}

	OVE_LOG_INF("=== Benchmark complete ===");
}

/* --- App entry point ---
 *
 * The bench creates+destroys kernel resources during measurement, so
 * `ove::run()` (which engages ove_heap_lock in zero-heap builds) is
 * NOT used here — we call `ove_thread_start_scheduler()` directly,
 * matching the C bench in tests/benchmarks/c/src/app.c.
 *
 * We bypass the C++ ove::Thread<> wrapper and call ove_thread_create
 * directly so that on bare-metal FreeRTOS the thread descriptor lives
 * in BSS (file-scope static) rather than triggering libstdc++
 * thread-safe-static-init machinery (which pulls in
 * __gnu_cxx::recursive_init_error vtable + typeinfo and would fail
 * the minimal-overhead audit).  The C bench uses the same approach.
 */

/* Static thread storage + stack — using `ove_thread_init` (mode-
 * agnostic, works in both heap and zero-heap modes) instead of
 * `ove_thread_create` (heap-only) so the bench app builds in both
 * modes from one source. */
OVE_THREAD_DEFINE(bench_runner_storage, 8192);
static ove_thread_t bench_runner_handle;
static const struct ove_thread_desc bench_runner_desc = {
	.name = "bench_run",
	.entry = benchmark_runner,
	.arg = nullptr,
	.priority = OVE_PRIO_NORMAL,
	.stack_size = 8192,
	.stack = bench_runner_storage_stack,
};

OVE_MAIN()
{
	OVE_LOG_INF("Benchmark app: init");

	int ret = ove_thread_init(&bench_runner_handle, &bench_runner_storage, &bench_runner_desc);
	if (ret != OVE_OK) {
		OVE_LOG_ERR("Failed to init benchmark thread: %d", ret);
		return;
	}

	ove_thread_start_scheduler();

	OVE_LOG_INF("Benchmark app: shutdown");
}
