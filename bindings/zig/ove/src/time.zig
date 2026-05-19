// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Time utilities — typed `Duration` and `Instant` with saturating arithmetic.
//!
//! `Duration.millis(N)` / `.secs(N)` / `.nanos(N)` produce timeouts that
//! every bounded-wait API in the binding accepts. `Instant.now()` reads the
//! monotonic clock; `Instant + Duration` yields a deadline for `*Until`
//! variants. The raw FFI sentinel `WAIT_FOREVER` is also re-exported for
//! direct C-call use.

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

// Prefer the typed `Duration` constructors for bounded waits:
//   try queue.sendFor(&item, .millis(100));
//   try mutex.lockFor(.secs(5));
// Raw u64 nanosecond timeouts are only used at the FFI boundary now.

/// Return the current monotonic time in nanoseconds.
///
/// Like `getNs()` but skips the error-mapping branch — the substrate's
/// `ove_time_get_ns` is infallible on every supported backend.  Use
/// this when composing raw nanosecond deadlines; the typed deadline
/// path uses `Instant.now().addDuration(...)` paired with `lockUntil`.
pub inline fn nowSteadyNs() u64 {
    var out: u64 = 0;
    _ = c.ove_time_get_ns(&out);
    return out;
}

/// Typed monotonic timestamp for `*Until(deadline)` methods.
///
/// Wraps a nanosecond count from the substrate's steady clock.
/// Construct only via `Instant.now()`, the `.forever` sentinel, or
/// arithmetic on an existing `Instant` — direct `@as(Instant, u64)`
/// is rejected at compile time by the `pub fn` boundary.  Prevents
/// the wrong-epoch / wrong-unit footgun:
/// `std.time.nanoTimestamp()` (wall clock), `ove.time.getUs()`
/// (wrong unit), or relative offsets all fail the type check at the
/// call site.
///
/// ```zig
/// const deadline = ove.time.Instant.now().addDuration(.millis(100));
/// try mtx.lockUntil(deadline);
/// try mtx.lockUntil(.forever);  // sentinel — block indefinitely
/// ```
pub const Instant = struct {
    ns: u64,

    /// Read the substrate's steady clock.  Infallible.
    pub inline fn now() Instant {
        return .{ .ns = nowSteadyNs() };
    }

    /// Sentinel "wait indefinitely" — maps to OVE_WAIT_FOREVER.  Pass
    /// to any `*Until` method to block forever via the deadline path.
    pub const forever: Instant = .{ .ns = std.math.maxInt(u64) };

    /// Offset this instant by `d`.  Saturating add — does not wrap.
    pub inline fn addDuration(self: Instant, d: Duration) Instant {
        return .{ .ns = self.ns +| d.ns };
    }

    /// Duration between `self` and an earlier `other`.  Saturating
    /// subtract — returns `.zero` if `other` is later than `self`.
    pub inline fn sub(self: Instant, other: Instant) Duration {
        return .{ .ns = self.ns -| other.ns };
    }
};

/// Convert an absolute deadline to the remaining timeout in
/// nanoseconds, preserving the `OVE_WAIT_FOREVER` sentinel.  Returns
/// 0 when the deadline is in the past.
///
/// Used internally by every binding's `*Until` variant.  The substrate
/// exposes the same helper as `ove_time_deadline_to_timeout_ns` (a
/// `static inline` in `<ove/time.h>` that `@cImport` doesn't surface).
pub inline fn deadlineToTimeoutNs(deadline: Instant) u64 {
    if (deadline.ns == @import("error.zig").wait_forever) return deadline.ns;
    const now_ns = nowSteadyNs();
    return if (deadline.ns > now_ns) deadline.ns - now_ns else 0;
}
