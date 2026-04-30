// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

/**
 * @file bench.hpp
 * @brief Safe C++ wrappers for the shared C benchmark harness.
 *
 * The harness lives in `tests/benchmarks/c/src/bench_harness.c` and consumes
 * per-suite `extern "C" const bench_suite_t bench_suite_<name>` symbols.
 *
 * Apps author cases from safe C++ `void()` free functions or stateless
 * lambdas via `bench::case_` and aggregate them into suites via
 * `bench::suite_def` + `OVE_BENCH_SUITE(...)`. The macros and
 * templates hide every `extern "C"` trampoline so app code stays free of
 * C-ABI plumbing.
 */

#pragma once

#include <cstdint>

extern "C" {
#include "benchmark.h"
}

namespace bench
{

/** Mirrors C `bench_type_t`. */
enum class Type : int {
	latency = 0,
	throughput = 1,
	memory = 2,
};

/** @brief Spec describing a bench case — pass to `case_` at compile time. */
struct CaseSpec {
	const char *name;	      /**< Case name (for harness output). */
	Type kind;		      /**< Measurement kind (latency/throughput/memory). */
	void (*run)();		      /**< Per-iteration body — required. */
	void (*setup)() = nullptr;    /**< Optional one-time setup. */
	void (*teardown)() = nullptr; /**< Optional one-time teardown. */
	uint32_t iterations = 0;      /**< Iteration count (0 = harness default). */
	uint32_t inner_iters =
		0; /**< For sub-µs cases: ×N inner reps per timestamp pair (0 = 1). */
};

namespace detail
{

/** @brief C-ABI trampoline forwarding to the spec's `run` callback. */
template <const CaseSpec &Spec> inline void run_trampoline(void *)
{
	Spec.run();
}

/** @brief C-ABI trampoline forwarding to the spec's `setup` callback (no-op when null). */
template <const CaseSpec &Spec> inline void setup_trampoline(void *)
{
	if (Spec.setup)
		Spec.setup();
}

/** @brief C-ABI trampoline forwarding to the spec's `teardown` callback (no-op when null). */
template <const CaseSpec &Spec> inline void teardown_trampoline(void *)
{
	if (Spec.teardown)
		Spec.teardown();
}

} /* namespace detail */

/**
 * @brief Build a `bench_case_t` from a compile-time `CaseSpec`.
 *
 * Generates direct C-ABI trampolines forwarding to the spec's
 * safe C++ `void()` callbacks. Intended for use in a `constexpr`/`static`
 * context where the spec itself is a `constexpr`.
 */
template <const CaseSpec &Spec> constexpr ::bench_case_t case_()
{
	// Always wire the setup/teardown trampoline — the trampoline itself is a
	// no-op when the spec didn't provide a fn. Avoids a compile-time warning
	// that "address of non-nullptr fn is never null".
	return ::bench_case_t{
		.name = Spec.name,
		.type = static_cast<::bench_type_t>(Spec.kind),
		.setup = &detail::setup_trampoline<Spec>,
		.run = &detail::run_trampoline<Spec>,
		.teardown = &detail::teardown_trampoline<Spec>,
		.iterations = Spec.iterations,
		.inner_iters = Spec.inner_iters,
	};
}

/** @brief Spec describing a bench suite. */
struct SuiteSpec {
	const char *name;	     /**< Suite name (for harness output). */
	bool (*enabled)();	     /**< Gate predicate — suite runs when it returns true. */
	const ::bench_case_t *cases; /**< Array of cases, length `case_count`. */
	unsigned int case_count;     /**< Number of entries in `cases`. */
};

namespace detail
{

/** @brief C-ABI trampoline forwarding the suite's `bool` gate to the harness' `int`. */
template <const SuiteSpec &Spec> inline int enabled_trampoline()
{
	return Spec.enabled() ? 1 : 0;
}

} /* namespace detail */

/** @brief Build a `bench_suite_t` from a compile-time `SuiteSpec`. */
template <const SuiteSpec &Spec> constexpr ::bench_suite_t suite_def()
{
	return ::bench_suite_t{
		.name = Spec.name,
		.is_enabled = &detail::enabled_trampoline<Spec>,
		.cases = Spec.cases,
		.case_count = Spec.case_count,
	};
}

/**
 * @brief Run every case in `suite` through the C harness.
 *
 * Collects per-case results into a fixed scratch buffer so JSON output
 * (when CONFIG_OVE_BENCHMARK_OUTPUT_JSON=y) can emit a single suite
 * envelope after all cases run.  MAX_CASES_PER_SUITE matches the value
 * in tests/benchmarks/c/src/app.c — bumped together if any suite ever
 * grows beyond it.
 */
static constexpr unsigned MAX_CASES_PER_SUITE = 32;

inline void run_suite(const ::bench_suite_t &suite)
{
	if (suite.is_enabled && suite.is_enabled() == 0)
		return;
	::bench_print_header(suite.name);
	/* Static (BSS) — 32 × bench_result_t ≈ 2.8 KB, too large for the
	 * runner thread's 8 KB stack on Cortex-M.  C++17 inline-variable
	 * semantics give all TUs a single shared copy, which is fine because
	 * suites run sequentially on a single bench thread. */
	static ::bench_result_t results[MAX_CASES_PER_SUITE];
	for (unsigned i = 0; i < MAX_CASES_PER_SUITE; ++i)
		results[i] = ::bench_result_t{};
	unsigned n = suite.case_count;
	if (n > MAX_CASES_PER_SUITE)
		n = MAX_CASES_PER_SUITE;
	for (unsigned i = 0; i < n; ++i) {
		::bench_run_case(&suite.cases[i], &results[i]);
		::bench_print_result(&suite.cases[i], &results[i]);
	}
	::bench_print_footer();
#if CONFIG_OVE_BENCHMARK_OUTPUT_JSON
	::bench_emit_suite_json(&suite, suite.cases, results, n);
#endif
}

} /* namespace bench */

/**
 * @def OVE_BENCH_SUITE(symbol, name_lit, enabled_fn, cases_array)
 * @brief Declare `extern "C" const bench_suite_t <symbol>` from safe inputs.
 *
 * `cases_array` must be a `constexpr` array of `bench_case_t` (typically
 * built via `bench::case_<Spec>()` entries).
 *
 * Example:
 * @code
 * static void time_get_us_overhead_run() { (void)ove::time::get_us(); }
 * static bool time_is_enabled() { return true; }
 *
 * static constexpr bench::CaseSpec time_get_us_spec{
 *     "time_get_us_overhead", bench::Type::latency, &time_get_us_overhead_run};
 * static constexpr bench_case_t time_cases[] = {
 *     bench::case_<time_get_us_spec>(),
 *     // ...
 * };
 *
 * OVE_BENCH_SUITE(bench_suite_time, "time", time_is_enabled, time_cases)
 * @endcode
 */
#define OVE_BENCH_SUITE(symbol, name_lit, enabled_fn, cases_array)            \
	static constexpr ::bench::SuiteSpec _##symbol##_spec{                 \
		.name = (name_lit),                                           \
		.enabled = (enabled_fn),                                      \
		.cases = (cases_array),                                       \
		.case_count = sizeof(cases_array) / sizeof((cases_array)[0]), \
	};                                                                    \
	extern "C" const ::bench_suite_t symbol = ::bench::suite_def<_##symbol##_spec>();
