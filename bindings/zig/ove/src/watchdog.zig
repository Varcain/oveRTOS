// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

/// Hardware watchdog timer.
///
/// ```zig
/// var wd: ove.Watchdog = undefined;
/// try wd.init(5000);
/// defer wd.deinit();
/// try wd.start();
/// // periodically: try wd.feed();
/// ```
pub const Watchdog = struct {
    storage: pin.Storage(c.ove_watchdog_storage_t),
    handle: c.ove_watchdog_t,
    tracker: pin.Tracker,

    pub fn init(self: *Watchdog, timeout_ms: u32) Error!void {
        self.storage = pin.zeroStorage(c.ove_watchdog_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_watchdog_create(&self.handle, timeout_ms));
        } else {
            try err.fromCode(c.ove_watchdog_init(&self.handle, &self.storage, timeout_ms));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Watchdog) void {
        self.tracker.assertSame(self, "ove.Watchdog");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_watchdog_destroy(self.handle)
        else
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
