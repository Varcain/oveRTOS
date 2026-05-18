// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// Time utilities (trailing underscore avoids std.time clash)

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Typed duration value, used throughout the binding for bounded waits.
///
/// Carries nanoseconds; constructors disambiguate the unit at the call
/// site so `queue.sendFor(item, .millis(100))` is the only honest reading.
/// Saturating multiplication (`*|`) avoids u64 overflow at construction
/// when a caller passes very large values.
///
/// ```zig
/// try mutex.lockFor(.millis(50));
/// try sem.timedWait(.secs(5));
/// try queue.sendFor(&item, .micros(250));
/// ```
pub const Duration = struct {
    ns: u64,

    /// Construct from a raw nanosecond count.
    pub inline fn nanos(n: u64) Duration {
        return .{ .ns = n };
    }
    /// Construct from microseconds.
    pub inline fn micros(us: u64) Duration {
        return .{ .ns = us *| std.time.ns_per_us };
    }
    /// Construct from milliseconds.
    pub inline fn millis(ms: u64) Duration {
        return .{ .ns = ms *| std.time.ns_per_ms };
    }
    /// Construct from seconds.
    pub inline fn secs(s: u64) Duration {
        return .{ .ns = s *| std.time.ns_per_s };
    }

    /// Zero duration — a non-blocking probe.  Prefer the primitive's
    /// `tryX` method for intent.
    pub const zero: Duration = .{ .ns = 0 };
};

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

/// Return the current monotonic time in nanoseconds.
///
/// Like `getNs()` but skips the error-mapping branch — the substrate's
/// `ove_time_get_ns` is infallible on every supported backend.  Use
/// this when composing deadlines for `lockUntil` / `takeUntil` etc.:
///   const deadline = ove.time.nowSteadyNs() + 100 * std.time.ns_per_ms;
///   try mutex.lockUntil(deadline);
pub inline fn nowSteadyNs() u64 {
    var out: u64 = 0;
    _ = c.ove_time_get_ns(&out);
    return out;
}

/// Convert an absolute steady-clock deadline to the remaining duration,
/// preserving the `OVE_WAIT_FOREVER` sentinel (== `u64` max).  Returns 0
/// when the deadline is in the past.
///
/// Used internally by every binding's `*Until` variant.  The substrate
/// exposes the same helper as `ove_time_deadline_to_timeout_ns` (a
/// `static inline` in `<ove/time.h>` that `@cImport` doesn't surface).
pub inline fn deadlineToTimeoutNs(deadline_ns: u64) u64 {
    if (deadline_ns == @import("error.zig").wait_forever) return deadline_ns;
    const now = nowSteadyNs();
    return if (deadline_ns > now) deadline_ns - now else 0;
}
