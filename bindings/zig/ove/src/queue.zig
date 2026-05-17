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

/// Type-safe message queue.
///
/// Generic on element type `T` and capacity `N`.
///
/// In heap mode the wrapper is a single handle; the kernel allocates the
/// backing buffer:
///
/// ```zig
/// var q = try ove.Queue(u32, 8).create();
/// defer q.deinit();
/// try q.send(&42, ove.wait_forever);
/// const v = try q.receive(ove.wait_forever);
/// ```
///
/// In zero-heap mode the backing buffer + queue storage live as struct
/// fields; two-phase init is required:
///
/// ```zig
/// var q: ove.Queue(u32, 8) = undefined;
/// try q.init();
/// defer q.deinit();
/// ```
pub fn Queue(comptime T: type, comptime N: comptime_int) type {
    return if (pin.zero_heap) ZeroHeapQueue(T, N) else HeapQueue(T, N);
}

fn HeapQueue(comptime T: type, comptime N: comptime_int) type {
    return struct {
        const Self = @This();

        handle: c.ove_queue_t,

        pub fn create() Error!Self {
            var h: c.ove_queue_t = null;
            try err.fromCode(c.ove_queue_create(&h, @sizeOf(T), N));
            return .{ .handle = h };
        }

        pub fn deinit(self: Self) void {
            if (self.handle == null) return;
            c.ove_queue_destroy(self.handle);
        }

        pub inline fn send(self: Self, item: *const T, timeout_ns: u64) Error!void {
            try err.fromCode(c.ove_queue_send(self.handle, @ptrCast(item), timeout_ns));
        }

        pub inline fn sendUntil(self: Self, item: *const T, deadline_ns: u64) Error!void {
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            try err.fromCode(c.ove_queue_send(self.handle, @ptrCast(item), t));
        }

        pub inline fn receive(self: Self, timeout_ns: u64) Error!T {
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive(self.handle, @ptrCast(&val), timeout_ns));
            return val;
        }

        pub inline fn receiveUntil(self: Self, deadline_ns: u64) Error!T {
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive(self.handle, @ptrCast(&val), t));
            return val;
        }

        pub inline fn sendFromIsr(self: Self, item: *const T) Error!void {
            try err.fromCode(c.ove_queue_send_from_isr(self.handle, @ptrCast(item)));
        }

        pub inline fn receiveFromIsr(self: Self) Error!T {
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive_from_isr(self.handle, @ptrCast(&val)));
            return val;
        }
    };
}

fn ZeroHeapQueue(comptime T: type, comptime N: comptime_int) type {
    return struct {
        const Self = @This();

        buffer: [N * @sizeOf(T)]u8,
        storage: c.ove_queue_storage_t,
        handle: c.ove_queue_t,
        tracker: pin.Tracker,

        pub fn init(self: *Self) Error!void {
            self.buffer = [_]u8{0} ** (N * @sizeOf(T));
            self.storage = std.mem.zeroes(c.ove_queue_storage_t);
            self.handle = null;
            self.tracker = .{};
            try err.fromCode(c.ove_queue_init(&self.handle, &self.storage, &self.buffer, @sizeOf(T), N));
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Queue");
            if (self.handle == null) return;
            c.ove_queue_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub inline fn send(self: *Self, item: *const T, timeout_ns: u64) Error!void {
            self.tracker.assertSame(self, "ove.Queue");
            try err.fromCode(c.ove_queue_send(self.handle, @ptrCast(item), timeout_ns));
        }

        pub inline fn sendUntil(self: *Self, item: *const T, deadline_ns: u64) Error!void {
            self.tracker.assertSame(self, "ove.Queue");
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            try err.fromCode(c.ove_queue_send(self.handle, @ptrCast(item), t));
        }

        pub inline fn receive(self: *Self, timeout_ns: u64) Error!T {
            self.tracker.assertSame(self, "ove.Queue");
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive(self.handle, @ptrCast(&val), timeout_ns));
            return val;
        }

        pub inline fn receiveUntil(self: *Self, deadline_ns: u64) Error!T {
            self.tracker.assertSame(self, "ove.Queue");
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive(self.handle, @ptrCast(&val), t));
            return val;
        }

        pub fn sendFromIsr(self: *Self, item: *const T) Error!void {
            try err.fromCode(c.ove_queue_send_from_isr(self.handle, @ptrCast(item)));
        }

        pub fn receiveFromIsr(self: *Self) Error!T {
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive_from_isr(self.handle, @ptrCast(&val)));
            return val;
        }
    };
}
