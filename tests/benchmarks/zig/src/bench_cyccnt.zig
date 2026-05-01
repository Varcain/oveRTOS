// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Bench-only DWT cycle-counter timer (Zig binding).
//!
//! Mirrors `tests/benchmarks/c/include/bench_cyccnt.h`.  On ARMv7-M /
//! ARMv7E-M (Cortex-M3/M4/M7) `read()` is a single dereference of a
//! `*volatile u32` at `0xE0001004` — exactly one ARM `LDR` after the
//! Zig backend's optimisation pipeline, the same shape the C inline
//! emits.
//!
//! On non-ARM targets (POSIX, sim) `available` is false and the C
//! harness's fallback (`ove_time_get_ns`) is what runs.
//!
//! Lives inside the bench tree only.  Not part of the public `ove`
//! Zig binding.

const builtin = @import("builtin");

/// True iff this build's `read()` is backed by DWT (i.e. ARMv7-M).
pub const available = switch (builtin.cpu.arch) {
    .arm, .armeb, .thumb, .thumbeb => true,
    else => false,
};

/// CPU clock used for cycle→ns conversion. Override with the build flag
/// `-Dbench_cyccnt_hz=<N>` if the target's CPU is not 216 MHz.
pub const hz: u32 = 216_000_000;

const demcr: *volatile u32 = @ptrFromInt(0xE000EDFC);
const dwt_ctrl: *volatile u32 = @ptrFromInt(0xE0001000);
const dwt_cyccnt: *volatile u32 = @ptrFromInt(0xE0001004);

/// Enable the DWT cycle counter. Idempotent — safe to call repeatedly.
pub fn init() void {
    if (!available) return;
    demcr.* |= (1 << 24);
    dwt_cyccnt.* = 0;
    dwt_ctrl.* |= 1;
}

/// Read the 32-bit free-running cycle counter. One `LDR` post-codegen.
pub inline fn read() u32 {
    if (!available) return 0;
    return dwt_cyccnt.*;
}

/// Convert a 32-bit cycle delta (modulo-2^32 unsigned subtract) to ns.
pub inline fn diffNs(start: u32, end: u32) u64 {
    const cycles: u32 = end -% start;
    return @as(u64, cycles) * 1_000_000_000 / @as(u64, hz);
}
