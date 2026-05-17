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

/// Variable-length byte stream buffer for inter-task data transfer.
///
/// Generic on the buffer capacity in bytes.
///
/// In heap mode the wrapper is a single handle; the kernel allocates the
/// backing buffer:
///
/// ```zig
/// var s = try ove.Stream(256).create(1);  // trigger=1
/// defer s.deinit();
/// ```
///
/// In zero-heap mode the buffer is embedded as a struct field; two-phase
/// init is required:
///
/// ```zig
/// var s: ove.Stream(256) = undefined;
/// try s.init(1);
/// defer s.deinit();
/// ```
pub fn Stream(comptime size: usize) type {
    return if (pin.zero_heap) ZeroHeapStream(size) else HeapStream(size);
}

fn HeapStream(comptime size: usize) type {
    return struct {
        const Self = @This();

        handle: c.ove_stream_t,

        /// `trigger` is the minimum bytes a receiver waits for before
        /// unblocking (0 or 1 = "wake on any byte").
        pub fn create(trigger: usize) Error!Self {
            var h: c.ove_stream_t = null;
            try err.fromCode(c.ove_stream_create(&h, size, trigger));
            return .{ .handle = h };
        }

        pub fn deinit(self: Self) void {
            if (self.handle == null) return;
            c.ove_stream_destroy(self.handle);
        }

        pub inline fn send(self: Self, data: []const u8, timeout_ns: u64) Error!usize {
            var sent: usize = 0;
            try err.fromCode(c.ove_stream_send(self.handle, data.ptr, data.len, timeout_ns, &sent));
            return sent;
        }

        pub inline fn receive(self: Self, buf: []u8, timeout_ns: u64) Error!usize {
            var received: usize = 0;
            try err.fromCode(c.ove_stream_receive(self.handle, buf.ptr, buf.len, timeout_ns, &received));
            return received;
        }

        pub fn sendFromIsr(self: Self, data: []const u8) Error!usize {
            var sent: usize = 0;
            try err.fromCode(c.ove_stream_send_from_isr(self.handle, data.ptr, data.len, &sent));
            return sent;
        }

        pub fn receiveFromIsr(self: Self, buf: []u8) Error!usize {
            var received: usize = 0;
            try err.fromCode(c.ove_stream_receive_from_isr(self.handle, buf.ptr, buf.len, &received));
            return received;
        }

        pub fn reset(self: Self) Error!void {
            try err.fromCode(c.ove_stream_reset(self.handle));
        }

        pub fn bytesAvailable(self: Self) usize {
            return c.ove_stream_bytes_available(self.handle);
        }
    };
}

fn ZeroHeapStream(comptime size: usize) type {
    return struct {
        const Self = @This();

        buffer: [size + 1]u8,
        storage: c.ove_stream_storage_t,
        handle: c.ove_stream_t,
        tracker: pin.Tracker,

        /// `trigger` is the minimum bytes a receiver waits for before
        /// unblocking (0 or 1 = "wake on any byte").
        pub fn init(self: *Self, trigger: usize) Error!void {
            self.buffer = [_]u8{0} ** (size + 1);
            self.storage = std.mem.zeroes(c.ove_stream_storage_t);
            self.handle = null;
            self.tracker = .{};
            try err.fromCode(c.ove_stream_init(
                &self.handle,
                &self.storage,
                &self.buffer,
                size,
                trigger,
            ));
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Stream");
            if (self.handle == null) return;
            c.ove_stream_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub inline fn send(self: *Self, data: []const u8, timeout_ns: u64) Error!usize {
            self.tracker.assertSame(self, "ove.Stream");
            var sent: usize = 0;
            try err.fromCode(c.ove_stream_send(self.handle, data.ptr, data.len, timeout_ns, &sent));
            return sent;
        }

        pub inline fn receive(self: *Self, buf: []u8, timeout_ns: u64) Error!usize {
            self.tracker.assertSame(self, "ove.Stream");
            var received: usize = 0;
            try err.fromCode(c.ove_stream_receive(self.handle, buf.ptr, buf.len, timeout_ns, &received));
            return received;
        }

        pub fn sendFromIsr(self: *Self, data: []const u8) Error!usize {
            var sent: usize = 0;
            try err.fromCode(c.ove_stream_send_from_isr(self.handle, data.ptr, data.len, &sent));
            return sent;
        }

        pub fn receiveFromIsr(self: *Self, buf: []u8) Error!usize {
            var received: usize = 0;
            try err.fromCode(c.ove_stream_receive_from_isr(self.handle, buf.ptr, buf.len, &received));
            return received;
        }

        pub fn reset(self: *Self) Error!void {
            self.tracker.assertSame(self, "ove.Stream");
            try err.fromCode(c.ove_stream_reset(self.handle));
        }

        pub fn bytesAvailable(self: *Self) usize {
            self.tracker.assertSame(self, "ove.Stream");
            return c.ove_stream_bytes_available(self.handle);
        }
    };
}
