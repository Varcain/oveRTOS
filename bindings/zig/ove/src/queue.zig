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

// Per-operation narrow error sets (A3).  See sync.zig for the rationale.
/// Error set for `Queue.send*` and `sendFromIsr`.
pub const SendError = error{ QueueFull, Timeout };
/// Error set for `Queue.receive*` and `receiveFromIsr`.
pub const RecvError = error{ QueueEmpty, Timeout };

inline fn mapSendError(comptime ctx: []const u8, rc: c_int) SendError {
    return switch (rc) {
        c.OVE_ERR_QUEUE_FULL => error.QueueFull,
        c.OVE_ERR_TIMEOUT => error.Timeout,
        else => std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc}),
    };
}

inline fn mapRecvError(comptime ctx: []const u8, rc: c_int) RecvError {
    return switch (rc) {
        c.OVE_ERR_QUEUE_EMPTY => error.QueueEmpty,
        c.OVE_ERR_TIMEOUT => error.Timeout,
        else => std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc}),
    };
}

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

        pub inline fn send(self: Self, item: *const T, timeout_ns: u64) SendError!void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), timeout_ns);
            if (rc < 0) return mapSendError("Queue.send", rc);
        }

        pub inline fn sendUntil(self: Self, item: *const T, deadline_ns: u64) SendError!void {
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), t);
            if (rc < 0) return mapSendError("Queue.sendUntil", rc);
        }

        pub inline fn receive(self: Self, timeout_ns: u64) RecvError!T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), timeout_ns);
            if (rc < 0) return mapRecvError("Queue.receive", rc);
            return val;
        }

        pub inline fn receiveUntil(self: Self, deadline_ns: u64) RecvError!T {
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), t);
            if (rc < 0) return mapRecvError("Queue.receiveUntil", rc);
            return val;
        }

        pub inline fn sendFromIsr(self: Self, item: *const T) SendError!void {
            const rc = c.ove_queue_send_from_isr(self.handle, @ptrCast(item));
            if (rc < 0) return mapSendError("Queue.sendFromIsr", rc);
        }

        pub inline fn receiveFromIsr(self: Self) RecvError!T {
            var val: T = undefined;
            const rc = c.ove_queue_receive_from_isr(self.handle, @ptrCast(&val));
            if (rc < 0) return mapRecvError("Queue.receiveFromIsr", rc);
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

        pub inline fn send(self: *Self, item: *const T, timeout_ns: u64) SendError!void {
            self.tracker.assertSame(self, "ove.Queue");
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), timeout_ns);
            if (rc < 0) return mapSendError("Queue.send", rc);
        }

        pub inline fn sendUntil(self: *Self, item: *const T, deadline_ns: u64) SendError!void {
            self.tracker.assertSame(self, "ove.Queue");
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), t);
            if (rc < 0) return mapSendError("Queue.sendUntil", rc);
        }

        pub inline fn receive(self: *Self, timeout_ns: u64) RecvError!T {
            self.tracker.assertSame(self, "ove.Queue");
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), timeout_ns);
            if (rc < 0) return mapRecvError("Queue.receive", rc);
            return val;
        }

        pub inline fn receiveUntil(self: *Self, deadline_ns: u64) RecvError!T {
            self.tracker.assertSame(self, "ove.Queue");
            const t = @import("time.zig").deadlineToTimeoutNs(deadline_ns);
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), t);
            if (rc < 0) return mapRecvError("Queue.receiveUntil", rc);
            return val;
        }

        pub fn sendFromIsr(self: *Self, item: *const T) SendError!void {
            const rc = c.ove_queue_send_from_isr(self.handle, @ptrCast(item));
            if (rc < 0) return mapSendError("Queue.sendFromIsr", rc);
        }

        pub fn receiveFromIsr(self: *Self) RecvError!T {
            var val: T = undefined;
            const rc = c.ove_queue_receive_from_isr(self.handle, @ptrCast(&val));
            if (rc < 0) return mapRecvError("Queue.receiveFromIsr", rc);
            return val;
        }
    };
}
