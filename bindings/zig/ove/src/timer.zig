// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Software timer — `Timer.create(allocator, cfg, callback, args)` with
//! `.periodic` or `.one_shot` mode.
//!
//! Wraps `ove/timer.h`. Callbacks run on the substrate's timer service thread
//! (FreeRTOS timer task, Zephyr system workqueue, …) — they must be
//! non-blocking. Available when `CONFIG_OVE_TIMER` is enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Mode for `create()`'s `mode` arg.  `.periodic` re-fires every period;
/// `.one_shot` fires once and stops.
pub const Mode = enum { periodic, one_shot };

/// Spawn-time configuration for `Timer.create`.  Pass as `.{ ... }` —
/// `mode` defaults to `.periodic`.
///
/// ```zig
/// .{ .period_ms = 100 }                         // periodic
/// .{ .period_ms = 50, .mode = .one_shot }       // fires once
/// ```
pub const Config = struct {
    period_ms: u32,
    mode: Mode = .periodic,
};

fn validateCallback(comptime Cb: type, comptime ArgsT: type) struct {
    user_param_count: usize,
} {
    const cb_info = @typeInfo(Cb);
    if (cb_info != .@"fn") {
        @compileError("ove.Timer.create: callback must be a function, got " ++ @typeName(Cb));
    }
    const fn_info = cb_info.@"fn";
    if (fn_info.return_type != null and fn_info.return_type.? != void) {
        @compileError("ove.Timer.create: callback must return void");
    }

    const args_info = @typeInfo(ArgsT);
    const args_field_count = if (args_info == .@"struct")
        args_info.@"struct".fields.len
    else
        @compileError("ove.Timer.create: args must be a tuple, got " ++ @typeName(ArgsT));

    if (fn_info.params.len != args_field_count) {
        @compileError(std.fmt.comptimePrint(
            "ove.Timer.create: callback takes {d} params but args has {d} elements",
            .{ fn_info.params.len, args_field_count },
        ));
    }
    // Limit: substrate passes a single `user_data: *anyopaque` — we
    // support 0 or 1 user params.  Multi-arg captures would need a
    // separately-allocated context tuple — not implemented today.
    if (fn_info.params.len > 1) {
        @compileError("ove.Timer.create: callback may take at most 1 user param (Thread-style)");
    }
    return .{ .user_param_count = fn_info.params.len };
}

/// Software timer.
///
/// Storage lives in allocator-managed memory; the substrate's `_init`
/// path runs against it.  Callback is `anytype`+`args` — mirrors
/// `std.Thread.spawn` and `ove.Thread.spawn`.
///
/// ```zig
/// // Plain callback:
/// var t = try ove.Timer.create(allocator, .{ .period_ms = 1000 }, tick, .{});
/// defer t.deinit();
///
/// // With a context pointer:
/// var t = try ove.Timer.create(
///     allocator,
///     .{ .period_ms = 50, .mode = .one_shot },
///     onCount, .{&counter},
/// );
/// ```
pub const Timer = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_timer_t,
    storage: *c.ove_timer_storage_t,

    pub fn create(
        allocator: std.mem.Allocator,
        comptime cfg: Config,
        comptime callback: anytype,
        args: anytype,
    ) Error!Timer {
        const info = comptime validateCallback(@TypeOf(callback), @TypeOf(args));

        const Tramp = if (info.user_param_count == 0) struct {
            fn invoke(_: c.ove_timer_t, _: ?*anyopaque) callconv(.c) void {
                callback();
            }
        } else struct {
            fn invoke(_: c.ove_timer_t, user_data: ?*anyopaque) callconv(.c) void {
                const ParamT = @typeInfo(@TypeOf(callback)).@"fn".params[0].type.?;
                const p: ParamT = @ptrCast(@alignCast(user_data));
                callback(p);
            }
        };

        const user_data: ?*anyopaque = if (info.user_param_count == 0)
            null
        else
            @ptrCast(args[0]);

        const storage = try allocator.create(c.ove_timer_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_timer_storage_t);
        var h: c.ove_timer_t = null;
        const one_shot_int: c_int = if (cfg.mode == .one_shot) 1 else 0;
        try err.fromCode(c.ove_timer_init(
            &h,
            storage,
            &Tramp.invoke,
            user_data,
            cfg.period_ms,
            one_shot_int,
        ));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: Timer) void {
        if (self.handle != null) c.ove_timer_deinit(self.handle);
        self.allocator.destroy(self.storage);
    }

    /// Arm the timer.  The first fire happens `cfg.period_ms` from
    /// this call.
    pub inline fn start(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_start(self.handle));
    }

    /// Disarm the timer.  A fire already in progress runs to
    /// completion; subsequent periodic ticks are cancelled.
    pub inline fn stop(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_stop(self.handle));
    }

    /// Re-arm a running timer; the period restart counts from now.
    pub inline fn reset(self: Timer) Error!void {
        try err.fromCode(c.ove_timer_reset(self.handle));
    }
};
