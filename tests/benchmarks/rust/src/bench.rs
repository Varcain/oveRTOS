// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Safe wrappers for the shared C benchmark harness.
//!
//! The harness lives in `tests/benchmarks/c/src/bench_harness.c` and consumes
//! per-suite `#[no_mangle] pub static bench_suite_<name>: CBenchSuite`
//! symbols. Each suite groups `CBenchCase` entries with setup / run /
//! teardown trampolines. These C-ABI types mirror `bench_case_t` /
//! `bench_suite_t` in `tests/benchmarks/c/include/benchmark.h`.
//!
//! Use the [`crate::bench_case!`] and [`crate::bench_suite!`] macros to
//! declare suites from safe Rust `fn()` setup / run / teardown helpers —
//! the macros generate the C-ABI trampolines internally so app code
//! never writes `unsafe extern "C"`.

// This module wraps the C harness FFI; the rest of the bench crate
// stays under `#![deny(unsafe_code)]`.
#![allow(unsafe_code)]

/// Benchmark classification — mirrors the C `bench_type_t` enum.
#[repr(C)]
#[derive(Clone, Copy)]
pub enum BenchType {
    Latency = 0,
    Throughput = 1,
    Memory = 2,
}

/// Benchmark result published by the harness. Mirrors `bench_result_t`
/// in `tests/benchmarks/c/include/benchmark.h` — the field order and
/// types must stay byte-compatible with the C struct, otherwise
/// bench_run_case() will write past the end of this struct and corrupt
/// the stack.
#[repr(C)]
pub struct BenchResult {
    pub min_ns: u64,
    pub max_ns: u64,
    pub total_ns: u64,
    pub count: u32,
    pub ops_per_sec: u32,
    pub heap_delta: i32,
    /// Set when CONFIG_OVE_BENCHMARK_PERCENTILES, else 0.
    pub p50_ns: u64,
    pub p95_ns: u64,
    pub p99_ns: u64,
    /// Top 1% of samples dropped — robust against scheduler jitter.
    pub trimmed_mean_ns: u64,
    /// Fixed-point: stddev_ns × 1000.
    pub stddev_ns_q: u64,
}

/// C-ABI bench case descriptor — mirrors `bench_case_t`.
///
/// Constructed through the [`crate::bench_case!`] macro; the generated
/// trampolines adapt safe `fn()` callbacks to the `unsafe extern "C" fn(*mut c_void)`
/// signatures the harness expects.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CBenchCase {
    pub name: *const core::ffi::c_char,
    pub bench_type: BenchType,
    pub setup: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub run: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub teardown: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub iterations: u32,
    /// For sub-µs ops: ×N inner reps per timestamp pair (0 = treat as 1).
    pub inner_iters: u32,
}

// SAFETY: CBenchCase contains only function pointers and a `'static` name
// pointer — no interior mutability, safe to share across threads.
unsafe impl Sync for CBenchCase {}

/// C-ABI bench suite descriptor — mirrors `bench_suite_t`.
#[repr(C)]
pub struct CBenchSuite {
    pub name: *const core::ffi::c_char,
    pub is_enabled: Option<unsafe extern "C" fn() -> i32>,
    pub cases: *const CBenchCase,
    pub case_count: u32,
}

unsafe impl Sync for CBenchSuite {}

unsafe extern "C" {
    /// Run a single benchmark case, writing results into `*result`.
    pub fn bench_run_case(bc: *const CBenchCase, result: *mut BenchResult);
    pub fn bench_print_header(suite_name: *const core::ffi::c_char);
    pub fn bench_print_result(bc: *const CBenchCase, result: *const BenchResult);
    pub fn bench_print_footer();

    /// Native pthread baseline suite — defined in
    /// `tests/benchmarks/c/src/bench_native_posix.c`.  Re-exported here so
    /// safe Rust apps with `#![deny(unsafe_code)]` can include it in
    /// their suite list via [`native_posix_suite`] without writing
    /// their own `unsafe extern` blocks.
    static bench_suite_native_posix: CBenchSuite;

    /// Native FreeRTOS baseline suite — defined in
    /// `tests/benchmarks/c/src/bench_native_freertos.c`.  Same accessor
    /// pattern as `native_posix_suite`; both are always linked, but
    /// each suite's `is_enabled` returns 0 on the wrong backend so
    /// only the active baseline runs.
    static bench_suite_native_freertos: CBenchSuite;
    /// Emit machine-readable JSON for an entire suite — parsed by
    /// scripts/bench_compare.py for cross-binding deltas.  Only linked
    /// when CONFIG_OVE_BENCHMARK_OUTPUT_JSON=y in the app's defconfig
    /// (which is the default for all benchmark apps).
    pub fn bench_emit_suite_json(
        suite: *const CBenchSuite,
        cases: *const CBenchCase,
        results: *const BenchResult,
        n: u32,
    );
}

/// Maximum number of cases per suite — must match the C side
/// (tests/benchmarks/c/src/app.c MAX_CASES_PER_SUITE).
const MAX_CASES_PER_SUITE: usize = 32;

/// Safe accessor for the native pthread baseline suite. Returns a
/// reference to the C-defined `bench_suite_native_posix` symbol.
pub fn native_posix_suite() -> &'static CBenchSuite {
    // SAFETY: `bench_suite_native_posix` is a `'static` C-side const
    // CBenchSuite — it has no interior mutability and outlives the
    // program. The address is fixed at link time.
    unsafe { &bench_suite_native_posix }
}

/// Safe accessor for the native FreeRTOS baseline suite.
pub fn native_freertos_suite() -> &'static CBenchSuite {
    // SAFETY: same as `native_posix_suite` — link-time-fixed const.
    unsafe { &bench_suite_native_freertos }
}

/// Run every case in `suite` through the C harness, printing header,
/// per-case results, footer, and (if compiled in) a JSON envelope.
///
/// The `results` scratch buffer is held in BSS rather than on the
/// stack — 32 × BenchResult ≈ 2.5 KB is too large for the runner
/// thread's 8 KB stack on Cortex-M.  Suites run sequentially on a
/// single bench thread, so no synchronisation is required.
pub fn run_suite(suite: &'static CBenchSuite) {
    use core::cell::UnsafeCell;
    struct ResultsBuf(UnsafeCell<[BenchResult; MAX_CASES_PER_SUITE]>);
    unsafe impl Sync for ResultsBuf {}

    static RESULTS: ResultsBuf = ResultsBuf(UnsafeCell::new(
        // SAFETY: BenchResult is `#[repr(C)]` with all-integer fields,
        // an all-zero bit pattern is a valid value.
        unsafe { core::mem::zeroed() },
    ));

    if let Some(enabled_fn) = suite.is_enabled {
        let enabled = unsafe { enabled_fn() };
        if enabled == 0 {
            return;
        }
    }

    let n = (suite.case_count as usize).min(MAX_CASES_PER_SUITE);
    let results: &mut [BenchResult; MAX_CASES_PER_SUITE] =
        unsafe { &mut *RESULTS.0.get() };
    for i in 0..MAX_CASES_PER_SUITE {
        results[i] = unsafe { core::mem::zeroed() };
    }

    unsafe {
        bench_print_header(suite.name);
        for i in 0..n {
            let case = suite.cases.add(i);
            bench_run_case(case, &mut results[i]);
            bench_print_result(case, &results[i]);
        }
        bench_print_footer();

        bench_emit_suite_json(
            suite as *const _,
            suite.cases,
            results.as_ptr(),
            n as u32,
        );
    }
    let _ = results;
}
