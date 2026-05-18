// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Mode for `create()`'s `mode` arg.  `.periodic` re-fires every period;
/// `.one_shot` fires once and stops.
pub const Mode = enum { periodic, one_shot };

/// Software timer.
///
/// Storage lives in allocator-managed memory; the substrate's `_init`
/// path runs against it.  Works uniformly in heap and zero-heap builds.
///
/// ```zig
/// var t = try ove.Timer.create(allocator, myCallback, 1000, .periodic);
/// defer t.deinit();
/// try t.start();
/// ```
pub const Timer = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_timer_t,
    storage: *c.ove_timer_storage_t,

    /// Create with a Zig callback (no context).
    pub fn create(
        allocator: std.mem.Allocator,
        comptime callback: fn () void,
        period_ms: u32,
        mode: Mode,
    ) Error!Timer {
        const Tramp = struct {
            fn invoke(_: c.ove_timer_t, _: ?*anyopaque) callconv(.c) void {
                callback();
            }
        };
        const storage = try allocator.create(c.ove_timer_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_timer_storage_t);
        var h: c.ove_timer_t = null;
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_init(
            &h,
            storage,
            &Tramp.invoke,
            null,
            period_ms,
            one_shot_int,
        ));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    /// Create with a typed context pointer.  `ctx` must outlive the timer.
    pub fn createWithContext(
        allocator: std.mem.Allocator,
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
        const storage = try allocator.create(c.ove_timer_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_timer_storage_t);
        var h: c.ove_timer_t = null;
        const one_shot_int: c_int = if (mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_init(
            &h,
            storage,
            &Tramp.invoke,
            @ptrCast(ctx),
            period_ms,
            one_shot_int,
        ));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: Timer) void {
        if (self.handle != null) c.ove_timer_deinit(self.handle);
        self.allocator.destroy(self.storage);
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
