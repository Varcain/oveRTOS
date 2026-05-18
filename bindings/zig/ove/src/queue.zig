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
const Duration = @import("time.zig").Duration;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

// Per-operation narrow error sets.  See sync.zig for the rationale.
/// Error set for `Queue.sendFor` and `sendFromIsr`.  `send()` is
/// forever-blocking and infallible after a successful `create()`.
pub const SendError = error{ QueueFull, Timeout };
/// Error set for `Queue.recvFor` and `receiveFromIsr`.  `recv()` is
/// forever-blocking and infallible.
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

inline fn panicOnRc(comptime ctx: []const u8, rc: c_int) void {
    if (rc < 0) std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc});
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

        /// Forever-blocking send.  Infallible after `create()`.
        pub inline fn send(self: Self, item: *const T) void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), WAIT_FOREVER);
            panicOnRc("Queue.send", rc);
        }

        /// Non-blocking send — fails fast with `error.QueueFull` if full.
        pub inline fn trySend(self: Self, item: *const T) error{QueueFull}!void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), 0);
            if (rc >= 0) return;
            if (rc == c.OVE_ERR_QUEUE_FULL or rc == c.OVE_ERR_TIMEOUT) return error.QueueFull;
            std.debug.panic("ove.Queue.trySend: unexpected substrate rc {d}", .{rc});
        }

        /// Bounded-duration send.
        pub inline fn sendFor(self: Self, item: *const T, d: Duration) SendError!void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), d.ns);
            if (rc < 0) return mapSendError("Queue.sendFor", rc);
        }

        /// Forever-blocking receive.  Infallible after `create()`.
        pub inline fn recv(self: Self) T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), WAIT_FOREVER);
            panicOnRc("Queue.recv", rc);
            return val;
        }

        /// Non-blocking receive — returns `null` if the queue is empty.
        pub inline fn tryRecv(self: Self) ?T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), 0);
            if (rc >= 0) return val;
            if (rc == c.OVE_ERR_QUEUE_EMPTY or rc == c.OVE_ERR_TIMEOUT) return null;
            std.debug.panic("ove.Queue.tryRecv: unexpected substrate rc {d}", .{rc});
        }

        /// Bounded-duration receive.
        pub inline fn recvFor(self: Self, d: Duration) RecvError!T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), d.ns);
            if (rc < 0) return mapRecvError("Queue.recvFor", rc);
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

        pub inline fn send(self: *Self, item: *const T) void {
            self.tracker.assertSame(self, "ove.Queue");
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), WAIT_FOREVER);
            panicOnRc("Queue.send", rc);
        }

        pub inline fn trySend(self: *Self, item: *const T) error{QueueFull}!void {
            self.tracker.assertSame(self, "ove.Queue");
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), 0);
            if (rc >= 0) return;
            if (rc == c.OVE_ERR_QUEUE_FULL or rc == c.OVE_ERR_TIMEOUT) return error.QueueFull;
            std.debug.panic("ove.Queue.trySend: unexpected substrate rc {d}", .{rc});
        }

        pub inline fn sendFor(self: *Self, item: *const T, d: Duration) SendError!void {
            self.tracker.assertSame(self, "ove.Queue");
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), d.ns);
            if (rc < 0) return mapSendError("Queue.sendFor", rc);
        }

        pub inline fn recv(self: *Self) T {
            self.tracker.assertSame(self, "ove.Queue");
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), WAIT_FOREVER);
            panicOnRc("Queue.recv", rc);
            return val;
        }

        pub inline fn tryRecv(self: *Self) ?T {
            self.tracker.assertSame(self, "ove.Queue");
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), 0);
            if (rc >= 0) return val;
            if (rc == c.OVE_ERR_QUEUE_EMPTY or rc == c.OVE_ERR_TIMEOUT) return null;
            std.debug.panic("ove.Queue.tryRecv: unexpected substrate rc {d}", .{rc});
        }

        pub inline fn recvFor(self: *Self, d: Duration) RecvError!T {
            self.tracker.assertSame(self, "ove.Queue");
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), d.ns);
            if (rc < 0) return mapRecvError("Queue.recvFor", rc);
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
