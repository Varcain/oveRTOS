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
#include <ove/bench.hpp>

/* --- Suite registry --- */

static const bench_suite_t *const suites[] = {
	&bench_suite_time,  &bench_suite_thread,     &bench_suite_sync,	     &bench_suite_queue,
	&bench_suite_timer, &bench_suite_eventgroup, &bench_suite_workqueue, &bench_suite_stream,
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
		ove::bench::run_suite(*suites[s]);
	}

	OVE_LOG_INF("=== Benchmark complete ===");
}

/* --- App entry point --- */

static ove::Thread<8192> runner_thread(benchmark_runner, nullptr, OVE_PRIO_NORMAL, "bench_run");

OVE_MAIN()
{
	OVE_LOG_INF("Benchmark app: init");

	ove::run();

	OVE_LOG_INF("Benchmark app: shutdown");
}
