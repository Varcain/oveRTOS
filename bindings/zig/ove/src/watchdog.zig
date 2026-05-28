// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Hardware watchdog — `Watchdog` with `start`/`feed`/`stop`.
//!
//! Wraps `ove/watchdog.h`. Once started, the watchdog must be fed before the
//! configured timeout elapses; missing a feed triggers a hardware reset.
//! Whether the timer is suspended during debugger halts is backend-specific.
//! The public `Watchdog` type is gated on `pin.zero_heap`. Available when
//! `CONFIG_OVE_WATCHDOG` is enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

/// Hardware watchdog timer.
///
/// Heap mode (value-returning create):
///
/// ```zig
/// var wd = try ove.Watchdog.create(5000);
/// defer wd.deinit();
/// try wd.start();
/// // periodically: try wd.feed();
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var wd: ove.Watchdog = undefined;
/// try wd.init(5000);
/// defer wd.deinit();
/// ```
pub const Watchdog = if (pin.zero_heap) ZeroHeapWatchdog else HeapWatchdog;

const HeapWatchdog = struct {
    handle: c.ove_watchdog_t,

    pub fn create(timeout_ms: u32) Error!Watchdog {
        var h: c.ove_watchdog_t = null;
        try err.fromCode(c.ove_watchdog_create(&h, timeout_ms));
        return .{ .handle = h };
    }

    pub fn deinit(self: *Watchdog) void {
        if (self.handle == null) return;
        c.ove_watchdog_destroy(self.handle);
        self.handle = null;
    }

    pub fn start(self: Watchdog) Error!void {
        try err.fromCode(c.ove_watchdog_start(self.handle));
    }

    pub fn stop(self: Watchdog) Error!void {
        try err.fromCode(c.ove_watchdog_stop(self.handle));
    }

    pub fn feed(self: Watchdog) Error!void {
        try err.fromCode(c.ove_watchdog_feed(self.handle));
    }
};

const ZeroHeapWatchdog = struct {
    storage: c.ove_watchdog_storage_t,
    handle: c.ove_watchdog_t,
    tracker: pin.Tracker,

    pub fn init(self: *Watchdog, timeout_ms: u32) Error!void {
        self.storage = std.mem.zeroes(c.ove_watchdog_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_watchdog_init(&self.handle, &self.storage, timeout_ms));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Watchdog) void {
        self.tracker.assertSame(self, "ove.Watchdog");
        if (self.handle == null) return;
        c.ove_watchdog_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn start(self: *Watchdog) Error!void {
        self.tracker.assertSame(self, "ove.Watchdog");
        try err.fromCode(c.ove_watchdog_start(self.handle));
    }

    pub fn stop(self: *Watchdog) Error!void {
        self.tracker.assertSame(self, "ove.Watchdog");
        try err.fromCode(c.ove_watchdog_stop(self.handle));
    }

    pub fn feed(self: *Watchdog) Error!void {
        self.tracker.assertSame(self, "ove.Watchdog");
        try err.fromCode(c.ove_watchdog_feed(self.handle));
    }
};
