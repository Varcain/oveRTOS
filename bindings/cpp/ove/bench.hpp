// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

/**
 * @file bench.hpp
 * @brief Safe C++ wrappers for the shared C benchmark harness.
 *
 * The harness lives in `apps/c/benchmark/src/bench_harness.c` and consumes
 * per-suite `extern "C" const bench_suite_t bench_suite_<name>` symbols.
 *
 * Apps author cases from safe C++ `void()` free functions or stateless
 * lambdas via `ove::bench::case_` and aggregate them into suites via
 * `ove::bench::suite_def` + `OVE_BENCH_SUITE(...)`. The macros and
 * templates hide every `extern "C"` trampoline so app code stays free of
 * C-ABI plumbing.
 */

#pragma once

#include <cstdint>

extern "C" {
#include "benchmark.h"
}

namespace ove::bench {

/** Mirrors C `bench_type_t`. */
enum class Type : int {
	latency = 0,
	throughput = 1,
	memory = 2,
};

/** @brief Spec describing a bench case — pass to `case_` at compile time. */
struct CaseSpec {
	const char *name;
	Type kind;
	void (*run)();
	void (*setup)() = nullptr;
	void (*teardown)() = nullptr;
	uint32_t iterations = 0;
};

namespace detail {

template <const CaseSpec &Spec>
inline void run_trampoline(void *) {
	Spec.run();
}

template <const CaseSpec &Spec>
inline void setup_trampoline(void *) {
	if (Spec.setup) Spec.setup();
}

template <const CaseSpec &Spec>
inline void teardown_trampoline(void *) {
	if (Spec.teardown) Spec.teardown();
}

} /* namespace detail */

/**
 * @brief Build a `bench_case_t` from a compile-time `CaseSpec`.
 *
 * Generates zero-overhead C-ABI trampolines forwarding to the spec's
 * safe C++ `void()` callbacks. Intended for use in a `constexpr`/`static`
 * context where the spec itself is a `constexpr`.
 */
template <const CaseSpec &Spec>
constexpr ::bench_case_t case_() {
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
	};
}

/** @brief Spec describing a bench suite. */
struct SuiteSpec {
	const char *name;
	bool (*enabled)();
	const ::bench_case_t *cases;
	unsigned int case_count;
};

namespace detail {

template <const SuiteSpec &Spec>
inline int enabled_trampoline() {
	return Spec.enabled() ? 1 : 0;
}

} /* namespace detail */

/** @brief Build a `bench_suite_t` from a compile-time `SuiteSpec`. */
template <const SuiteSpec &Spec>
constexpr ::bench_suite_t suite_def() {
	return ::bench_suite_t{
		.name = Spec.name,
		.is_enabled = &detail::enabled_trampoline<Spec>,
		.cases = Spec.cases,
		.case_count = Spec.case_count,
	};
}

/** @brief Run every case in `suite` through the C harness. */
inline void run_suite(const ::bench_suite_t &suite) {
	if (suite.is_enabled && suite.is_enabled() == 0) return;
	::bench_print_header(suite.name);
	for (unsigned i = 0; i < suite.case_count; ++i) {
		::bench_result_t result{};
		::bench_run_case(&suite.cases[i], &result);
		::bench_print_result(&suite.cases[i], &result);
	}
	::bench_print_footer();
}

} /* namespace ove::bench */

/**
 * @def OVE_BENCH_SUITE(symbol, name_lit, enabled_fn, cases_array)
 * @brief Declare `extern "C" const bench_suite_t <symbol>` from safe inputs.
 *
 * `cases_array` must be a `constexpr` array of `bench_case_t` (typically
 * built via `ove::bench::case_<Spec>()` entries).
 *
 * Example:
 * @code
 * static void time_get_us_overhead_run() { (void)ove::time::get_us(); }
 * static bool time_is_enabled() { return true; }
 *
 * static constexpr ove::bench::CaseSpec time_get_us_spec{
 *     "time_get_us_overhead", ove::bench::Type::latency, &time_get_us_overhead_run};
 * static constexpr bench_case_t time_cases[] = {
 *     ove::bench::case_<time_get_us_spec>(),
 *     // ...
 * };
 *
 * OVE_BENCH_SUITE(bench_suite_time, "time", time_is_enabled, time_cases)
 * @endcode
 */
#define OVE_BENCH_SUITE(symbol, name_lit, enabled_fn, cases_array)           \
	static constexpr ::ove::bench::SuiteSpec _##symbol##_spec{            \
		.name = (name_lit),                                           \
		.enabled = (enabled_fn),                                      \
		.cases = (cases_array),                                       \
		.case_count = sizeof(cases_array) / sizeof((cases_array)[0]), \
	};                                                                    \
	extern "C" const ::bench_suite_t symbol =                             \
		::ove::bench::suite_def<_##symbol##_spec>();
