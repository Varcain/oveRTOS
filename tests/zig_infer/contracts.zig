// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
const std = @import("std");
const builtin = @import("builtin");
const ove = @import("ove");
const c = ove.ffi;
const zero_heap = @hasDecl(c, "CONFIG_OVE_ZERO_HEAP");

// Metadata for the private same-layout tracker observer; no model executes.
export const infer_contract_layout: [8]usize = if (zero_heap) .{
    @sizeOf(ove.Model),                         @alignOf(ove.Model),                                                                    @offsetOf(ove.Model, "storage"),
    @offsetOf(ove.Model, "handle"),             @offsetOf(ove.Model, "tracker"),                                                        @sizeOf(@FieldType(ove.Model, "tracker")),
    @alignOf(@FieldType(ove.Model, "tracker")), if (builtin.mode == .Debug) @offsetOf(@FieldType(ove.Model, "tracker"), "addr") else 0,
} else .{ @sizeOf(ove.Model), @alignOf(ove.Model), 0, @offsetOf(ove.Model, "handle"), 0, 0, 0, 0 };

extern const infer_fixture_model: [64]u8 align(16);
extern fn infer_fixture_reset() void;
extern fn infer_fixture_failure(status: c_int) void;
extern fn infer_fixture_count(which: c_uint) c_uint;
extern fn infer_fixture_check(condition: bool, expression: [*:0]const u8) void;
extern fn infer_fixture_finish(name: [*:0]const u8) u8;
extern fn infer_fixture_header_unchanged() bool;
extern fn infer_fixture_capacity() usize;
extern fn infer_fixture_data() ?*const anyopaque;
extern fn infer_fixture_length() usize;
extern fn infer_fixture_arena() ?*anyopaque;

fn check(condition: bool, comptime description: [:0]const u8) void {
    infer_fixture_check(condition, description);
}

fn config(size: usize) c.ove_model_config {
    return .{ .model_data = &infer_fixture_model, .model_size = 64, .arena_size = size };
}

fn open(model: *ove.Model, arena: []u8, cfg: *const c.ove_model_config) ove.Error!void {
    if (zero_heap) {
        try model.init(arena, cfg);
    } else {
        model.* = try ove.Model.create(cfg);
    }
}

fn errorOf(result: ove.Error!void) usize {
    result catch |err| return @intFromError(err);
    return 0;
}

fn forwarding() void {
    for ([_]usize{ 129, 256 }) |size| {
        infer_fixture_reset();
        var arena: [256]u8 align(16) = @splat(0);
        var model: ove.Model = undefined;
        const cfg = config(size);
        open(&model, &arena, &cfg) catch {
            check(false, "valid forwarding setup");
            return;
        };
        defer model.deinit();
        check(infer_fixture_count(0) == @intFromBool(zero_heap), "expected init route");
        check(infer_fixture_count(1) == @intFromBool(!zero_heap), "expected create route");
        check(infer_fixture_capacity() == size, "exact requested capacity");
        check(infer_fixture_data() == cfg.model_data, "exact model pointer");
        check(infer_fixture_length() == 64, "exact model length");
        check(cfg.arena_size == size, "caller config unchanged");
        if (zero_heap) {
            check(infer_fixture_arena() == @as(*anyopaque, @ptrCast(&arena)), "exact arena address");
            if (builtin.mode == .Debug) check(model.tracker.addr == @as(?*const anyopaque, @ptrCast(&model)), "live tracker address");
        }
    }
}

fn extent() void {
    for ([_]usize{ 257, std.math.maxInt(usize) }) |size| {
        infer_fixture_reset();
        var arena: [256]u8 align(16) = @splat(0);
        var model: ove.Model = undefined;
        const cfg = config(size);
        const result = open(&model, &arena, &cfg);
        if (zero_heap) {
            check(errorOf(result) == @intFromError(error.InvalidParam), "oversized arena rejected");
            check(infer_fixture_count(0) == 0, "oversized request has no native call");
            // An unexpected parent success still owns a valid external fake
            // token. Close it once; no advertised span was ever traversed.
            if (result) |_| model.deinit() else |_| {}
        } else {
            if (result) |_| {
                check(infer_fixture_count(1) == 1, "heap capacity is independent");
                check(infer_fixture_capacity() == size, "heap preserves capacity");
                model.deinit();
            } else |_| check(false, "heap recording setup");
        }
    }
    if (zero_heap) {
        infer_fixture_reset();
        var arena: [256]u8 align(16) = @splat(0);
        var model: ove.Model = undefined;
        const cfg = config(1);
        const result = open(&model, arena[0..0], &cfg);
        check(errorOf(result) == @intFromError(error.InvalidParam), "empty arena rejected");
        check(infer_fixture_count(0) == 0, "empty arena has no native call");
        if (result) |_| model.deinit() else |_| {}

        infer_fixture_reset();
        infer_fixture_failure(c.OVE_ERR_INVALID_PARAM);
        const zero_cfg = config(0);
        const zero_result = open(&model, arena[0..0], &zero_cfg);
        check(errorOf(zero_result) == @intFromError(error.InvalidParam), "zero configured capacity error");
        check(infer_fixture_count(0) == 1, "zero capacity reaches native validation");
        check(infer_fixture_capacity() == 0, "zero capacity unchanged");
        // The failed first use has no state to inspect or clean up.
    }
}

fn errors() void {
    for ([_]c_int{ c.OVE_ERR_BUSY, c.OVE_ERR_INVALID_PARAM, c.OVE_ERR_NO_MEMORY, c.OVE_ERR_ML_FAILED }) |status| {
        infer_fixture_reset();
        infer_fixture_failure(status);
        var arena: [256]u8 align(16) = @splat(0);
        var model: ove.Model = undefined;
        const cfg = config(129);
        const result = open(&model, &arena, &cfg);
        const expected: usize = switch (status) {
            c.OVE_ERR_BUSY => @intFromError(error.Busy),
            c.OVE_ERR_INVALID_PARAM => @intFromError(error.InvalidParam),
            c.OVE_ERR_NO_MEMORY => @intFromError(error.OutOfMemory),
            c.OVE_ERR_ML_FAILED => @intFromError(error.MlFailed),
            else => unreachable,
        };
        check(errorOf(result) == expected, "native error mapping");
        check(infer_fixture_count(2) == 0, "failed first init grants no cleanup");
        // Never inspect the failed first-use object's fields or deinit it.
    }
}

fn busyLive() void {
    infer_fixture_reset();
    var arena: [256]u8 align(16) = @splat(0);
    var model: ove.Model = undefined;
    const cfg = config(129);
    open(&model, &arena, &cfg) catch {
        check(false, "live Busy setup");
        return;
    };
    const original = model.handle;
    const result = open(&model, &arena, &cfg);
    check(errorOf(result) == @intFromError(error.Busy), "second init reports Busy");
    check(model.handle == original, "Busy preserves the live handle");
    check(infer_fixture_header_unchanged(), "Busy preserves native record");
    if (zero_heap and builtin.mode == .Debug) {
        check(model.tracker.addr == @as(?*const anyopaque, @ptrCast(&model)), "Busy preserves tracker");
        if (model.tracker.addr != @as(?*const anyopaque, @ptrCast(&model))) return;
    }
    // Parent corruption remains a pure observer failure; avoid its invalid
    // cleanup state. No worker or actual resource exists in the C fake.
    if (model.handle != original) return;
    model.deinit();
    check(infer_fixture_count(2) == 1, "live owner closes exactly once");
}

fn busyCandidate() void {
    if (zero_heap) {
        infer_fixture_reset();
        infer_fixture_failure(c.OVE_ERR_BUSY);
        var arena: [256]u8 align(16) = @splat(0);
        // Every observed field is initialized; storage is opaque byte backing.
        var model = std.mem.zeroes(ove.Model);
        @memset(std.mem.asBytes(&model.storage), 0xa5);
        const before = std.mem.asBytes(&model.storage).*;
        const cfg = config(129);
        const result = open(&model, &arena, &cfg);
        check(errorOf(result) == @intFromError(error.Busy), "candidate Busy result");
        check(std.mem.eql(u8, &before, std.mem.asBytes(&model.storage)), "Busy preserves candidate storage");
        check(model.handle == null, "candidate null output unchanged");
        if (builtin.mode == .Debug) check(model.tracker.addr == null, "candidate tracker unchanged");
        check(infer_fixture_count(2) == 0, "candidate failure has no cleanup");
        // This initialized-byte observer does not grant a session on failure.
    }
}

fn closed(observer: bool) void {
    infer_fixture_reset();
    var arena: [256]u8 align(16) = @splat(0);
    var model: ove.Model = undefined;
    const cfg = config(256);
    open(&model, &arena, &cfg) catch {
        check(false, "close setup");
        return;
    };
    model.deinit();
    check(model.handle == null, "close clears handle");
    check(infer_fixture_count(2) == 1, "first close is native");
    if (observer) {
        check(infer_fixture_count(3) == 1, "live tracker observer active");
        check(infer_fixture_count(4) == 0, "live tracker valid");
        if (infer_fixture_count(3) != 1) return;
    }
    const calls = infer_fixture_count(3);
    const violations = infer_fixture_count(4);
    model.deinit();
    check(infer_fixture_count(2) == 1, "redundant close is not native");
    if (observer) {
        check(infer_fixture_count(3) == calls, "closed fast path skips tracker");
        check(infer_fixture_count(4) == violations, "closed fast path has no violation");
    }
    open(&model, &arena, &cfg) catch {
        check(false, "reuse after close");
        return;
    };
    model.deinit();
    check(infer_fixture_count(2) == 2, "reused owner closes once");
}

pub fn main(init: std.process.Init.Minimal) u8 {
    const args = init.args.vector;
    if (args.len != 2) return 2;
    const name = std.mem.span(args[1]);
    const Case = enum { forward, extent, errors, busy_live, busy_candidate, closed, closed_observer, canary };
    switch (std.meta.stringToEnum(Case, name) orelse return 2) {
        .forward => forwarding(),
        .extent => extent(),
        .errors => errors(),
        .busy_live => busyLive(),
        .busy_candidate => busyCandidate(),
        .closed => closed(false),
        .closed_observer => closed(true),
        .canary => {
            infer_fixture_reset();
            check(false, "explicit fixture failure canary");
            infer_fixture_reset();
        },
    }
    return infer_fixture_finish(args[1]);
}
