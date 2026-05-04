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
/// In heap mode the wrapper is a single handle returned by value:
///
/// ```zig
/// var t = try ove.Timer.create(myCallback, 1000, .periodic);
/// defer t.deinit();
/// try t.start();
/// ```
///
/// In zero-heap mode the storage is embedded; two-phase init is required:
///
/// ```zig
/// var t: ove.Timer = undefined;
/// try t.init(myCallback, 1000, .periodic);
/// defer t.deinit();
/// try t.start();
/// ```
pub const Timer = if (pin.zero_heap) ZeroHeapTimer else HeapTimer;

/// Mode for `init`/`create`'s `mode` arg.  `.periodic` re-fires every period;
/// `.one_shot` fires once and stops.
pub const Mode = enum { periodic, one_shot };

const HeapTimer = struct {
    handle: c.ove_timer_t,

    /// Create with a Zig callback (no context).
    pub fn create(
        comptime callback: fn () void,
        period_ms: u32,
        mode: Mode,
    ) Error!Timer {
        const Tramp = struct {
            fn invoke(_: c.ove_timer_t, _: ?*anyopaque) callconv(.c) void {
                callback();
            }
        };
        var h: c.ove_timer_t = null;
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_create(&h, &Tramp.invoke, null, period_ms, one_shot_int));
        return .{ .handle = h };
    }

    /// Create with a typed context pointer.  `ctx` must outlive the timer.
    pub fn createWithContext(
        comptime Context: type,
        ctx: *Context,
        comptime callback: fn (*Context) void,
        period_ms: u32,
        mode: Mode,
    ) Error!Timer {
        const Tramp = struct {
            fn invoke(_: c.ove_timer_t, user_data: ?*anyopaque) callconv(.c) void {
                const ptr: *Context = @ptrCast(@alignCast(user_data));
                callback(ptr);
            }
        };
        var h: c.ove_timer_t = null;
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_create(&h, &Tramp.invoke, @ptrCast(ctx), period_ms, one_shot_int));
        return .{ .handle = h };
    }

    pub fn deinit(self: Timer) void {
        if (self.handle == null) return;
        c.ove_timer_destroy(self.handle);
    }

    pub inline fn start(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_start(self.handle));
    }

    pub inline fn stop(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_stop(self.handle));
    }

    pub inline fn reset(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_reset(self.handle));
    }
};

const ZeroHeapTimer = struct {
    storage: c.ove_timer_storage_t,
    handle: c.ove_timer_t,
    tracker: pin.Tracker,

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
        self.storage = std.mem.zeroes(c.ove_timer_storage_t);
        self.handle = null;
        self.tracker = .{};
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_init(
            &self.handle,
            &self.storage,
            &Tramp.invoke,
            null,
            period_ms,
            one_shot_int,
        ));
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
        self.storage = std.mem.zeroes(c.ove_timer_storage_t);
        self.handle = null;
        self.tracker = .{};
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_init(
            &self.handle,
            &self.storage,
            &Tramp.invoke,
            @ptrCast(ctx),
            period_ms,
            one_shot_int,
        ));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Timer) void {
        self.tracker.assertSame(self, "ove.Timer");
        if (self.handle == null) return;
        c.ove_timer_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn start(self: *Timer) Error!void {
        self.tracker.assertSame(self, "ove.Timer");
        try err.fromCode(c.ove_timer_start(self.handle));
    }

    pub inline fn stop(self: *Timer) Error!void {
        self.tracker.assertSame(self, "ove.Timer");
        try err.fromCode(c.ove_timer_stop(self.handle));
    }

    pub inline fn reset(self: *Timer) Error!void {
        self.tracker.assertSame(self, "ove.Timer");
        try err.fromCode(c.ove_timer_reset(self.handle));
    }
};
