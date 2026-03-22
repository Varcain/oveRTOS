// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Hardware watchdog timer handle.
///
/// The watchdog resets the system if `feed()` is not called within
/// `timeout_ms` milliseconds of the last feed (or `start()`).
/// Supports both heap and zero-heap backends.
pub const Watchdog = struct {
    handle: c.ove_watchdog_t,

    /// Create a watchdog with the given timeout in milliseconds.
    ///
    /// The watchdog does not begin counting until `start()` is called.
    /// In zero-heap mode, the storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS or hardware fails to create the watchdog.
    pub fn create(timeout_ms: u32) Error!Watchdog {
        var h: c.ove_watchdog_t = null;
        if (comptime @hasDecl(c, "ove_watchdog_create")) {
            try err.fromCode(c.ove_watchdog_create(&h, timeout_ms));
        } else {
            const S = struct {
                var storage: c.ove_watchdog_storage_t = std.mem.zeroes(c.ove_watchdog_storage_t);
            };
            try err.fromCode(c.ove_watchdog_init(&h, &S.storage, timeout_ms));
        }
        return .{ .handle = h };
    }

    /// Destroy the watchdog and release underlying resources.
    ///
    /// Sets `handle` to null. The watchdog is implicitly stopped.
    /// Safe to call on an already-destroyed watchdog.
    pub fn destroy(self: *Watchdog) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_watchdog_destroy"))
            c.ove_watchdog_destroy(self.handle)
        else
            c.ove_watchdog_deinit(self.handle);
        self.handle = null;
    }

    /// Arm the watchdog and begin the timeout countdown.
    ///
    /// After calling `start()`, `feed()` must be called within `timeout_ms`
    /// milliseconds to prevent a system reset.
    /// Returns `Error` if the start operation fails.
    pub fn start(self: Watchdog) Error!void {
        try err.fromCode(c.ove_watchdog_start(self.handle));
    }

    /// Disarm the watchdog, stopping the timeout countdown.
    ///
    /// The system will not be reset until `start()` is called again.
    /// Returns `Error` if the stop operation fails.
    pub fn stop(self: Watchdog) Error!void {
        try err.fromCode(c.ove_watchdog_stop(self.handle));
    }

    /// Feed (pet/kick) the watchdog, resetting the timeout countdown.
    ///
    /// Must be called periodically before `timeout_ms` elapses to prevent
    /// a system reset. Returns `Error` if the feed operation fails.
    pub fn feed(self: Watchdog) Error!void {
        try err.fromCode(c.ove_watchdog_feed(self.handle));
    }
};
