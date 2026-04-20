// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Safe wrappers for the shared C benchmark harness.
//!
//! The harness lives in `apps/c/benchmark/src/bench_harness.c` and consumes
//! per-suite `#[no_mangle] pub static bench_suite_<name>: CBenchSuite`
//! symbols. Each suite groups `CBenchCase` entries with setup / run /
//! teardown trampolines. These C-ABI types mirror `bench_case_t` /
//! `bench_suite_t` in `apps/c/benchmark/include/benchmark.h`.
//!
//! Use the [`crate::bench_case!`] and [`crate::bench_suite!`] macros to
//! declare suites from safe Rust `fn()` setup / run / teardown helpers —
//! the macros generate the C-ABI trampolines internally so app code
//! never writes `unsafe extern "C"`.

/// Benchmark classification — mirrors the C `bench_type_t` enum.
#[repr(C)]
#[derive(Clone, Copy)]
pub enum BenchType {
    Latency = 0,
    Throughput = 1,
    Memory = 2,
}

/// Benchmark result published by the harness. Mirrors `bench_result_t`.
#[repr(C)]
pub struct BenchResult {
    pub min_ns: u64,
    pub max_ns: u64,
    pub total_ns: u64,
    pub count: u32,
    pub ops_per_sec: u32,
    pub heap_delta: i32,
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
}

/// Run every case in `suite` through the C harness, printing header,
/// per-case results, and footer. Safe wrapper around the C harness calls.
pub fn run_suite(suite: &'static CBenchSuite) {
    // Optional suite gate — if `is_enabled` returns 0, skip.
    if let Some(enabled_fn) = suite.is_enabled {
        let enabled = unsafe { enabled_fn() };
        if enabled == 0 {
            return;
        }
    }

    unsafe {
        bench_print_header(suite.name);
        for i in 0..suite.case_count {
            let case = suite.cases.add(i as usize);
            let mut result: BenchResult = core::mem::zeroed();
            bench_run_case(case, &mut result);
            bench_print_result(case, &result);
        }
        bench_print_footer();
    }
}
