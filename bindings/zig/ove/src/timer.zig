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

/// Software timer.
///
/// ```zig
/// var t: ove.Timer = undefined;
/// try t.init(myCallback, 1000, .periodic);
/// defer t.deinit();
/// try t.start();
/// ```
///
/// See `ove/sync.zig` for the pinning contract — the wrapper must not be
/// moved or copied after `init()`.
pub const Timer = struct {
    storage: pin.Storage(c.ove_timer_storage_t),
    handle: c.ove_timer_t,
    tracker: pin.Tracker,

    /// Mode for `init`'s `mode` arg.  `.periodic` re-fires every period;
    /// `.one_shot` fires once and stops.
    pub const Mode = enum { periodic, one_shot };

    /// Initialise with a Zig callback (no context).
    pub fn init(
        self: *Timer,
        comptime callback: fn () void,
        period_ms: u32,
        mode: Mode,
    ) Error!void {
        const Tramp = struct {
            fn invoke(_: c.ove_timer_t, _: ?*anyopaque) callconv(.c) void {
                callback();
            }
        };
        self.storage = pin.zeroStorage(c.ove_timer_storage_t);
        self.handle = null;
        self.tracker = .{};
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_timer_create(
                &self.handle,
                &Tramp.invoke,
                null,
                period_ms,
                one_shot_int,
            ));
        } else {
            try err.fromCode(c.ove_timer_init(
                &self.handle,
                &self.storage,
                &Tramp.invoke,
                null,
                period_ms,
                one_shot_int,
            ));
        }
        self.tracker.record(self);
    }

    /// Initialise with a typed context pointer.  `ctx` must outlive the timer.
    pub fn initWithContext(
        self: *Timer,
        comptime Context: type,
        ctx: *Context,
        comptime callback: fn (*Context) void,
        period_ms: u32,
        mode: Mode,
    ) Error!void {
        const Tramp = struct {
            fn invoke(_: c.ove_timer_t, user_data: ?*anyopaque) callconv(.c) void {
                const ptr: *Context = @ptrCast(@alignCast(user_data));
                callback(ptr);
            }
        };
        self.storage = pin.zeroStorage(c.ove_timer_storage_t);
        self.handle = null;
        self.tracker = .{};
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_timer_create(
                &self.handle,
                &Tramp.invoke,
                @ptrCast(ctx),
                period_ms,
                one_shot_int,
            ));
        } else {
            try err.fromCode(c.ove_timer_init(
                &self.handle,
                &self.storage,
                &Tramp.invoke,
                @ptrCast(ctx),
                period_ms,
                one_shot_int,
            ));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Timer) void {
        self.tracker.assertSame(self, "ove.Timer");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_timer_destroy(self.handle)
        else
            c.ove_timer_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn start(self: *Timer) Error!void {
        self.tracker.assertSame(self, "ove.Timer");
        try err.fromCode(c.ove_timer_start(self.handle));
    }

    pub fn stop(self: *Timer) Error!void {
        self.tracker.assertSame(self, "ove.Timer");
        try err.fromCode(c.ove_timer_stop(self.handle));
    }

    pub fn reset(self: *Timer) Error!void {
        self.tracker.assertSame(self, "ove.Timer");
        try err.fromCode(c.ove_timer_reset(self.handle));
    }
};
