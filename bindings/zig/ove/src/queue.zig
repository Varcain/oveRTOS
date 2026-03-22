// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Type-safe message queue with comptime element type and capacity.
///
/// `T` is the element type for each message. `N` is the maximum number of
/// messages the queue can hold at once. Both heap and zero-heap backends
/// are supported — in zero-heap mode the storage and ring buffer live in
/// comptime-unique static variables, limiting the queue to one instance
/// per call site.
///
/// Example:
///     const MsgQueue = Queue(u32, 8);
///     var q = try MsgQueue.create();
///     defer q.destroy();
///     try q.send(&42, wait_forever);
pub fn Queue(comptime T: type, comptime N: comptime_int) type {
    return struct {
        handle: c.ove_queue_t,

        const Self = @This();

        /// Create and return a new queue.
        ///
        /// In zero-heap mode, the internal storage and buffer are allocated as
        /// comptime-unique static variables. Returns `Error` if the RTOS fails
        /// to create the queue.
        pub fn create() Error!Self {
            var h: c.ove_queue_t = null;
            if (comptime @hasDecl(c, "ove_queue_create")) {
                try err.fromCode(c.ove_queue_create(&h, @sizeOf(T), N));
            } else {
                const S = struct {
                    var storage: c.ove_queue_storage_t = std.mem.zeroes(c.ove_queue_storage_t);
                    var buffer: [N * @sizeOf(T)]u8 = [_]u8{0} ** (N * @sizeOf(T));
                };
                try err.fromCode(c.ove_queue_init(&h, &S.storage, &S.buffer, @sizeOf(T), N));
            }
            return .{ .handle = h };
        }

        /// Destroy the queue and release underlying RTOS resources.
        ///
        /// Sets `handle` to null. Safe to call on an already-destroyed queue.
        pub fn destroy(self: *Self) void {
            if (self.handle == null) return;
            if (comptime @hasDecl(c, "ove_queue_destroy"))
                c.ove_queue_destroy(self.handle)
            else
                c.ove_queue_deinit(self.handle);
            self.handle = null;
        }

        /// Send a message to the queue, blocking up to `timeout_ms` milliseconds.
        ///
        /// `item` is copied by value into the queue. Use `wait_forever` to block
        /// indefinitely. Returns `Error.QueueFull` if the timeout expires and the
        /// queue is still full, or `Error.Timeout` on a timed-out block.
        pub fn send(self: Self, item: *const T, timeout_ms: u32) Error!void {
            try err.fromCode(c.ove_queue_send(
                self.handle,
                @ptrCast(item),
                timeout_ms,
            ));
        }

        /// Receive a message from the queue, blocking up to `timeout_ms` milliseconds.
        ///
        /// Returns the dequeued value. Returns `Error.Timeout` if no message
        /// arrives within the timeout.
        pub fn receive(self: Self, timeout_ms: u32) Error!T {
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive(
                self.handle,
                @ptrCast(&val),
                timeout_ms,
            ));
            return val;
        }

        /// Send a message from an interrupt service routine (non-blocking).
        ///
        /// Returns `Error.QueueFull` immediately if the queue has no space.
        /// Must only be called from ISR context.
        pub fn sendFromIsr(self: Self, item: *const T) Error!void {
            try err.fromCode(c.ove_queue_send_from_isr(
                self.handle,
                @ptrCast(item),
            ));
        }

        /// Receive a message from an interrupt service routine (non-blocking).
        ///
        /// Returns the dequeued value, or `Error.Timeout` if the queue is empty.
        /// Must only be called from ISR context.
        pub fn receiveFromIsr(self: Self) Error!T {
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive_from_isr(
                self.handle,
                @ptrCast(&val),
            ));
            return val;
        }
    };
}
