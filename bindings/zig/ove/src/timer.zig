// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Software timer handle.
///
/// Wraps the opaque `ove_timer_t` handle. Supports both heap and zero-heap
/// backends. Create with `create()` (simple callback) or `createWithContext()`
/// (typed context pointer), then call `start()` to arm the timer.
pub const Timer = struct {
    handle: c.ove_timer_t,

    /// Create a timer with a Zig callback (no context).
    pub fn create(
        comptime callback: fn () void,
        period_ms: u32,
        one_shot: bool,
    ) Error!Timer {
        const Trampoline = struct {
            fn invoke(_: c.ove_timer_t, _: ?*anyopaque) callconv(.c) void {
                callback();
            }
        };

        const one_shot_int: c_int = if (one_shot) 1 else 0;
        var h: c.ove_timer_t = null;
        if (comptime @hasDecl(c, "ove_timer_create")) {
            try err.fromCode(c.ove_timer_create(
                &h,
                &Trampoline.invoke,
                null,
                period_ms,
                one_shot_int,
            ));
        } else {
            const S = struct {
                var storage: c.ove_timer_storage_t = std.mem.zeroes(c.ove_timer_storage_t);
            };
            try err.fromCode(c.ove_timer_init(
                &h,
                &S.storage,
                &Trampoline.invoke,
                null,
                period_ms,
                one_shot_int,
            ));
        }
        return .{ .handle = h };
    }

    /// Create a timer with a typed context pointer.
    pub fn createWithContext(
        comptime Context: type,
        ctx: *Context,
        comptime callback: fn (*Context) void,
        period_ms: u32,
        one_shot: bool,
    ) Error!Timer {
        const Trampoline = struct {
            fn invoke(_: c.ove_timer_t, user_data: ?*anyopaque) callconv(.c) void {
                const ptr: *Context = @ptrCast(@alignCast(user_data));
                callback(ptr);
            }
        };

        const one_shot_int: c_int = if (one_shot) 1 else 0;
        var h: c.ove_timer_t = null;
        if (comptime @hasDecl(c, "ove_timer_create")) {
            try err.fromCode(c.ove_timer_create(
                &h,
                &Trampoline.invoke,
                @ptrCast(ctx),
                period_ms,
                one_shot_int,
            ));
        } else {
            const S = struct {
                var storage: c.ove_timer_storage_t = std.mem.zeroes(c.ove_timer_storage_t);
            };
            try err.fromCode(c.ove_timer_init(
                &h,
                &S.storage,
                &Trampoline.invoke,
                @ptrCast(ctx),
                period_ms,
                one_shot_int,
            ));
        }
        return .{ .handle = h };
    }

    /// Destroy the timer and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed timer.
    pub fn destroy(self: *Timer) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_timer_destroy"))
            c.ove_timer_destroy(self.handle)
        else
            c.ove_timer_deinit(self.handle);
        self.handle = null;
    }

    /// Arm the timer and begin counting down from the configured period.
    ///
    /// For a periodic timer, the callback fires repeatedly every `period_ms`.
    /// For a one-shot timer, the callback fires once and the timer stops.
    /// Returns `Error` if the RTOS fails to start the timer.
    pub fn start(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_start(self.handle));
    }

    /// Stop the timer. The callback will not fire until `start()` is called again.
    ///
    /// Returns `Error` if the RTOS fails to stop the timer.
    pub fn stop(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_stop(self.handle));
    }

    /// Restart the timer from zero, as if it had just been started.
    ///
    /// Useful for implementing watchdog-style "kick" patterns.
    /// Returns `Error` if the RTOS fails to reset the timer.
    pub fn reset(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_reset(self.handle));
    }
};
