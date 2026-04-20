// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Safe wrappers for the shared C benchmark harness.
//!
//! The harness lives in `apps/c/benchmark/src/bench_harness.c` and consumes
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

/// Mirrors C `bench_result_t`.
pub const BenchResult = extern struct {
    min_ns: u64 = 0,
    max_ns: u64 = 0,
    total_ns: u64 = 0,
    count: u32 = 0,
    ops_per_sec: u32 = 0,
    heap_delta: i32 = -1,
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

/// Run every case in `suite` through the C harness (header, results, footer).
pub fn runSuite(suite: *const CBenchSuite) void {
    if (suite.is_enabled) |en| {
        if (en() == 0) return;
    }
    bench_print_header(suite.name);
    var i: u32 = 0;
    while (i < suite.case_count) : (i += 1) {
        const bc: *const CBenchCase = &suite.cases[i];
        var result: BenchResult = .{};
        bench_run_case(bc, &result);
        bench_print_result(bc, &result);
    }
    bench_print_footer();
}
