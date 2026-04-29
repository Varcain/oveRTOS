// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Safe wrappers for the shared C benchmark harness.
//!
//! The harness lives in `tests/benchmarks/c/src/bench_harness.c` and consumes
//! per-suite `extern const bench_suite_t bench_suite_<name>` symbols. Apps
//! declare cases from safe Zig `fn` callbacks via [`case`] and aggregate
//! them into suites via [`makeSuite`] + `@export`; the comptime helpers
//! generate all `callconv(.c)` trampolines internally so app code stays
//! free of `callconv(.c)` / raw pointer plumbing.

const std = @import("std");

/// Mirrors C `bench_type_t`.
pub const BenchType = enum(c_int) {
    latency = 0,
    throughput = 1,
    memory = 2,
};

/// Mirrors C `bench_result_t` in `tests/benchmarks/c/include/benchmark.h`.
/// Field order and types must stay byte-compatible with the C struct,
/// otherwise bench_run_case() will write past the end and corrupt the
/// stack of the caller in main.zig.
pub const BenchResult = extern struct {
    min_ns: u64 = 0,
    max_ns: u64 = 0,
    total_ns: u64 = 0,
    count: u32 = 0,
    ops_per_sec: u32 = 0,
    heap_delta: i32 = -1,
    /// Set when CONFIG_OVE_BENCHMARK_PERCENTILES, else 0.
    p50_ns: u64 = 0,
    p95_ns: u64 = 0,
    p99_ns: u64 = 0,
    /// Top 1% of samples dropped — robust against scheduler jitter.
    trimmed_mean_ns: u64 = 0,
    /// Fixed-point: stddev_ns × 1000.
    stddev_ns_q: u64 = 0,
};

/// Mirrors C `bench_case_t`. Construct via [`case`] — do not hand-build.
pub const CBenchCase = extern struct {
    name: [*:0]const u8,
    bench_type: BenchType,
    setup: ?*const fn (?*anyopaque) callconv(.c) void,
    run: ?*const fn (?*anyopaque) callconv(.c) void,
    teardown: ?*const fn (?*anyopaque) callconv(.c) void,
    iterations: u32,
};

/// Mirrors C `bench_suite_t`. Construct via [`makeSuite`].
pub const CBenchSuite = extern struct {
    name: [*:0]const u8,
    is_enabled: ?*const fn () callconv(.c) c_int,
    cases: [*]const CBenchCase,
    case_count: u32,
};

extern fn bench_run_case(bc: *const CBenchCase, result: *BenchResult) void;
extern fn bench_print_header(suite_name: [*:0]const u8) void;
extern fn bench_print_result(bc: *const CBenchCase, result: *const BenchResult) void;
extern fn bench_print_footer() void;
/// Emit machine-readable JSON for an entire suite — parsed by
/// scripts/bench_compare.py.  Linked when CONFIG_OVE_BENCHMARK_OUTPUT_JSON=y
/// (default in every benchmark app.yaml).
extern fn bench_emit_suite_json(
    suite: *const CBenchSuite,
    cases: [*]const CBenchCase,
    results: [*]const BenchResult,
    n: u32,
) void;

/// Declarative spec for a single bench case. Pass to [`case`] at comptime.
pub const CaseSpec = struct {
    name: [*:0]const u8,
    kind: BenchType,
    run: *const fn () void,
    setup: ?*const fn () void = null,
    teardown: ?*const fn () void = null,
    iterations: u32 = 0,
};

/// Build a `CBenchCase` from a spec. Comptime — each invocation synthesises
/// unique `callconv(.c)` trampolines that forward to the safe Zig fns.
pub fn case(comptime spec: CaseSpec) CBenchCase {
    const Tramps = struct {
        fn runTramp(_: ?*anyopaque) callconv(.c) void {
            spec.run();
        }
        fn setupTramp(_: ?*anyopaque) callconv(.c) void {
            if (spec.setup) |s| s();
        }
        fn teardownTramp(_: ?*anyopaque) callconv(.c) void {
            if (spec.teardown) |t| t();
        }
    };
    return .{
        .name = spec.name,
        .bench_type = spec.kind,
        .setup = if (spec.setup != null) &Tramps.setupTramp else null,
        .run = &Tramps.runTramp,
        .teardown = if (spec.teardown != null) &Tramps.teardownTramp else null,
        .iterations = spec.iterations,
    };
}

/// Like [`case`], plus `@export`s the run trampoline under a stable
/// suite-scoped name (`zig_bench_run_<suite_tag>_<spec.name>`) so the
/// hotpath audit (`scripts/dump_hotpaths.py`) can locate it across
/// rebuilds.  Zig's anonymous numbered trampolines
/// (`bench.case.Tramps.runTramp.<N>`) are rebuild-volatile, so the
/// numbered symbol alone is not a stable audit target; this export is
/// an alias to the same address pinned to a deterministic name.
///
/// The suite_tag prevents collisions when bench-case names repeat
/// across suites (e.g. `create_destroy` exists in mutex / sem / queue
/// / timer / eventgroup / workqueue / stream — Zig's `@export` would
/// otherwise reject the duplicates).
pub fn caseAudited(comptime suite_tag: [:0]const u8, comptime spec: CaseSpec) CBenchCase {
    const Tramps = struct {
        fn runTramp(_: ?*anyopaque) callconv(.c) void {
            spec.run();
        }
        fn setupTramp(_: ?*anyopaque) callconv(.c) void {
            if (spec.setup) |s| s();
        }
        fn teardownTramp(_: ?*anyopaque) callconv(.c) void {
            if (spec.teardown) |t| t();
        }
    };
    @export(&Tramps.runTramp, .{
        .name = "zig_bench_run_" ++ suite_tag ++ "_" ++ std.mem.span(spec.name),
    });
    return .{
        .name = spec.name,
        .bench_type = spec.kind,
        .setup = if (spec.setup != null) &Tramps.setupTramp else null,
        .run = &Tramps.runTramp,
        .teardown = if (spec.teardown != null) &Tramps.teardownTramp else null,
        .iterations = spec.iterations,
    };
}

/// Declarative spec for a bench suite. Pass to [`makeSuite`] at comptime.
pub const SuiteSpec = struct {
    name: [*:0]const u8,
    enabled: *const fn () bool,
    cases: []const CBenchCase,
};

/// Build a `CBenchSuite` from a spec. Caller typically assigns the result
/// to a `const` and then `@export`s it with the `bench_suite_<name>` symbol
/// the C harness expects.
pub fn makeSuite(comptime spec: SuiteSpec) CBenchSuite {
    const Tramp = struct {
        fn enabledTramp() callconv(.c) c_int {
            return if (spec.enabled()) 1 else 0;
        }
    };
    return .{
        .name = spec.name,
        .is_enabled = &Tramp.enabledTramp,
        .cases = spec.cases.ptr,
        .case_count = @intCast(spec.cases.len),
    };
}

/// Maximum number of cases per suite — must match MAX_CASES_PER_SUITE
/// in tests/benchmarks/c/src/app.c (and the C++ binding's bench.hpp).
pub const MAX_CASES_PER_SUITE: usize = 32;

/// Run every case in `suite` through the C harness, collect per-case
/// results into a fixed scratch buffer, and emit a single suite JSON
/// envelope after printing the human ASCII table.
///
/// `results` is a file-scope static (BSS) — 32 × BenchResult ≈ 2.5 KB
/// is too large for the runner thread's 8 KB stack on Cortex-M.
/// Suites run sequentially on a single bench thread, so no
/// synchronisation is required.
var results_buf: [MAX_CASES_PER_SUITE]BenchResult = [_]BenchResult{.{}} ** MAX_CASES_PER_SUITE;

pub fn runSuite(suite: *const CBenchSuite) void {
    if (suite.is_enabled) |en| {
        if (en() == 0) return;
    }
    bench_print_header(suite.name);

    for (&results_buf) |*r| r.* = .{};
    const n: u32 = @min(suite.case_count, @as(u32, MAX_CASES_PER_SUITE));
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const bc: *const CBenchCase = &suite.cases[i];
        bench_run_case(bc, &results_buf[i]);
        bench_print_result(bc, &results_buf[i]);
    }
    bench_print_footer();
    bench_emit_suite_json(suite, suite.cases, &results_buf, n);
}
