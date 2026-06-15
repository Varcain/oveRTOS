// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Fixed-size FIFO message queue — `Queue(T, N)`.
//!
//! Wraps `ove/queue.h`. Items are copied by `memcpy(sizeof(T))` on send/recv,
//! so `T` must be trivially-copyable. Forever-blocking forms (`send`, `recv`)
//! return `void` / `T`; bounded forms (`sendFor`, `recvFor`, `sendUntil`,
//! `recvUntil`) take a typed `Duration`/`Instant` and surface a narrow error
//! set (`SendError`, `RecvError`). Available when `CONFIG_OVE_QUEUE` is
//! enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const time_mod = @import("time.zig");
const Duration = time_mod.Duration;
const Instant = time_mod.Instant;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

// Per-operation narrow error sets.
/// Error set returned by `Queue.sendFor` / `sendUntil` / `sendFromIsr`.
/// Bounded waits surface both substrate codes; ISR sends never see
/// `Timeout` in practice but the set is unified for ergonomic
/// `try`/`catch` composition.
pub const SendError = error{ QueueFull, Timeout };
/// Error set returned by `Queue.recvFor` / `recvUntil` / `receiveFromIsr`.
/// Bounded waits surface both substrate codes; ISR receives never see
/// `Timeout` in practice but the set is unified for ergonomic
/// `try`/`catch` composition.
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
/// Generic on element type `T` and capacity `N`.  Storage layout +
/// substrate handle live in allocator-managed memory; the wrapper carries
/// only an `Allocator` + two pointers.
///
/// ```zig
/// var q = try ove.Queue(u32, 8).create(allocator);
/// defer q.deinit();
/// const item: u32 = 42;
/// q.send(&item);
/// const v = q.recv();
/// ```
pub fn Queue(comptime T: type, comptime N: comptime_int) type {
    comptime {
        // Substrate copies items through `memcpy`; `T` with a
        // `deinit` method would silently leak its resources when
        // popped from the queue without the destructor running.
        // Catch the common mistake (queuing `ArrayList(u8)` or
        // similar resource-owning types) at instantiation time.
        // `@hasDecl` only works on struct/union/enum/opaque types —
        // skip primitives.
        const ti = @typeInfo(T);
        const has_decls = ti == .@"struct" or ti == .@"union" or ti == .@"enum" or ti == .@"opaque";
        if (has_decls and @hasDecl(T, "deinit")) {
            @compileError(
                "ove.Queue: element type " ++ @typeName(T) ++ " has a deinit() method.  " ++
                    "Items pass through memcpy — destructors never run.  Use a plain " ++
                    "data type, or wrap the resource in a heap pointer and queue the pointer.",
            );
        }
    }
    return struct {
        const Self = @This();

        const Backing = struct {
            buffer: [N * @sizeOf(T)]u8 align(@alignOf(T)),
            storage: c.ove_queue_storage_t,
        };

        allocator: std.mem.Allocator,
        handle: c.ove_queue_t,
        backing: ?*Backing,

        /// Allocate the queue's backing buffer + substrate-storage
        /// from `allocator` and `ove_queue_init` against it.
        pub fn create(allocator: std.mem.Allocator) Error!Self {
            const backing = try allocator.create(Backing);
            errdefer allocator.destroy(backing);
            backing.buffer = std.mem.zeroes([N * @sizeOf(T)]u8);
            backing.storage = std.mem.zeroes(c.ove_queue_storage_t);
            var h: c.ove_queue_t = null;
            try err.fromCode(c.ove_queue_init(&h, &backing.storage, &backing.buffer, @sizeOf(T), N));
            return .{ .allocator = allocator, .handle = h, .backing = backing };
        }

        pub fn deinit(self: *Self) void {
            if (self.handle) |handle| {
                c.ove_queue_deinit(handle);
                self.handle = null;
            }
            if (self.backing) |backing| {
                self.allocator.destroy(backing);
                self.backing = null;
            }
        }

        /// Forever-blocking send.  Infallible.
        pub inline fn send(self: Self, item: *const T) void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), WAIT_FOREVER);
            panicOnRc("Queue.send", rc);
        }

        /// Non-blocking enqueue.  Returns `error.QueueFull` when no slot
        /// is available (the substrate may surface this as either
        /// `OVE_ERR_QUEUE_FULL` or `OVE_ERR_TIMEOUT` for a 0-timeout
        /// send; both are normalised here).
        pub inline fn trySend(self: Self, item: *const T) error{QueueFull}!void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), 0);
            if (rc >= 0) return;
            if (rc == c.OVE_ERR_QUEUE_FULL or rc == c.OVE_ERR_TIMEOUT) return error.QueueFull;
            std.debug.panic("ove.Queue.trySend: unexpected substrate rc {d}", .{rc});
        }

        pub inline fn sendFor(self: Self, item: *const T, d: Duration) SendError!void {
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), d.ns);
            if (rc < 0) return mapSendError("Queue.sendFor", rc);
        }

        pub inline fn sendUntil(self: Self, item: *const T, deadline: Instant) SendError!void {
            const t = time_mod.deadlineToTimeoutNs(deadline);
            const rc = c.ove_queue_send(self.handle, @ptrCast(item), t);
            if (rc < 0) return mapSendError("Queue.sendUntil", rc);
        }

        pub inline fn recv(self: Self) T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), WAIT_FOREVER);
            panicOnRc("Queue.recv", rc);
            return val;
        }

        /// Non-blocking dequeue.  Returns `null` when no item is
        /// available (the substrate may surface this as either
        /// `OVE_ERR_QUEUE_EMPTY` or `OVE_ERR_TIMEOUT` for a 0-timeout
        /// receive; both are normalised here).
        pub inline fn tryRecv(self: Self) ?T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), 0);
            if (rc >= 0) return val;
            if (rc == c.OVE_ERR_QUEUE_EMPTY or rc == c.OVE_ERR_TIMEOUT) return null;
            std.debug.panic("ove.Queue.tryRecv: unexpected substrate rc {d}", .{rc});
        }

        pub inline fn recvFor(self: Self, d: Duration) RecvError!T {
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), d.ns);
            if (rc < 0) return mapRecvError("Queue.recvFor", rc);
            return val;
        }

        pub inline fn recvUntil(self: Self, deadline: Instant) RecvError!T {
            const t = time_mod.deadlineToTimeoutNs(deadline);
            var val: T = undefined;
            const rc = c.ove_queue_receive(self.handle, @ptrCast(&val), t);
            if (rc < 0) return mapRecvError("Queue.recvUntil", rc);
            return val;
        }

        /// Enqueue `item` from an ISR.  Non-blocking; returns
        /// `error.QueueFull` when the queue is at capacity.
        pub fn sendFromIsr(self: Self, item: *const T) SendError!void {
            const rc = c.ove_queue_send_from_isr(self.handle, @ptrCast(item));
            if (rc < 0) return mapSendError("Queue.sendFromIsr", rc);
        }

        /// Dequeue from an ISR.  Non-blocking; returns
        /// `error.QueueEmpty` when no item is available.
        pub fn receiveFromIsr(self: Self) RecvError!T {
            var val: T = undefined;
            const rc = c.ove_queue_receive_from_isr(self.handle, @ptrCast(&val));
            if (rc < 0) return mapRecvError("Queue.receiveFromIsr", rc);
            return val;
        }
    };
}
