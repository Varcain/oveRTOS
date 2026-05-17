// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// Time utilities (trailing underscore avoids std.time clash)

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Return the current system time in microseconds since boot.
///
/// Resolution depends on the underlying hardware timer. Returns `Error` if
/// the platform does not support microsecond-resolution time queries.
pub fn getUs() Error!u64 {
    var out: u64 = 0;
    try err.fromCode(c.ove_time_get_us(&out));
    return out;
}

/// Return the current system time in nanoseconds since boot.
///
/// Resolution depends on the underlying hardware timer. Returns `Error` if
/// the platform does not support nanosecond-resolution time queries.
pub fn getNs() Error!u64 {
    var out: u64 = 0;
    try err.fromCode(c.ove_time_get_ns(&out));
    return out;
}

/// Block the calling thread for at least `ms` milliseconds.
///
/// Yields the CPU to other threads during the delay. Actual delay may be
/// slightly longer depending on scheduler resolution.
pub inline fn delayMs(milliseconds: u32) void {
    c.ove_time_delay_ms(milliseconds);
}

/// Block the calling thread for at least `us` microseconds.
///
/// May be implemented as a busy-wait spin on platforms lacking sub-millisecond
/// sleep support. Prefer `delayMs()` for longer delays.
pub inline fn delayUs(microseconds: u32) void {
    c.ove_time_delay_us(microseconds);
}

// For timeout expressions, use Zig's stdlib constants directly — that's
// the idiomatic Zig pattern (mirrors std.time.sleep callers):
//   try queue.send(&item, 100 * std.time.ns_per_ms);
//   try mutex.lock(5 * std.time.ns_per_s);
