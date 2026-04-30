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
/// Generic on element type `T` and capacity `N`.  In zero-heap mode the
/// backing buffer + queue storage live as struct fields; in heap mode
/// they are zero-sized.
///
/// ```zig
/// var q: ove.Queue(u32, 8) = undefined;
/// try q.init();
/// defer q.deinit();
/// try q.send(&42, ove.wait_forever);
/// const v = try q.receive(ove.wait_forever);
/// ```
pub fn Queue(comptime T: type, comptime N: comptime_int) type {
    return struct {
        const Self = @This();

        buffer: if (pin.zero_heap) [N * @sizeOf(T)]u8 else void,
        storage: pin.Storage(c.ove_queue_storage_t),
        handle: c.ove_queue_t,
        tracker: pin.Tracker,

        pub fn init(self: *Self) Error!void {
            if (comptime pin.zero_heap) {
                self.buffer = [_]u8{0} ** (N * @sizeOf(T));
            } else {
                self.buffer = {};
            }
            self.storage = pin.zeroStorage(c.ove_queue_storage_t);
            self.handle = null;
            self.tracker = .{};
            if (comptime !pin.zero_heap) {
                try err.fromCode(c.ove_queue_create(&self.handle, @sizeOf(T), N));
            } else {
                try err.fromCode(c.ove_queue_init(&self.handle, &self.storage, &self.buffer, @sizeOf(T), N));
            }
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Queue");
            if (self.handle == null) return;
            if (comptime !pin.zero_heap)
                c.ove_queue_destroy(self.handle)
            else
                c.ove_queue_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub fn send(self: *Self, item: *const T, timeout_ms: u32) Error!void {
            self.tracker.assertSame(self, "ove.Queue");
            try err.fromCode(c.ove_queue_send(self.handle, @ptrCast(item), timeout_ms));
        }

        pub fn receive(self: *Self, timeout_ms: u32) Error!T {
            self.tracker.assertSame(self, "ove.Queue");
            var val: T = undefined;
            try err.fromCode(c.ove_queue_receive(self.handle, @ptrCast(&val), timeout_ms));
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
