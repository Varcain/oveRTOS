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
/// Generic on the buffer capacity in bytes — the ring buffer storage lives
/// inside the wrapper struct.  In zero-heap mode the buffer is allocated as
/// a struct field; in heap mode the field is zero-sized.
///
/// ```zig
/// var s: ove.Stream(256) = undefined;
/// try s.init(1);    // trigger=1: wake on any byte
/// defer s.deinit();
/// _ = try s.send(payload, ove.wait_forever);
/// _ = try s.receive(buf[0..], ove.wait_forever);
/// ```
pub fn Stream(comptime size: usize) type {
    return struct {
        const Self = @This();

        /// Backing ring buffer.  Zero-sized in heap mode.
        buffer: if (pin.zero_heap) [size + 1]u8 else void,
        storage: pin.Storage(c.ove_stream_storage_t),
        handle: c.ove_stream_t,
        tracker: pin.Tracker,

        /// Initialise.  `trigger` is the minimum bytes a receiver waits for
        /// before unblocking (0 or 1 = "wake on any byte").
        pub fn init(self: *Self, trigger: usize) Error!void {
            if (comptime pin.zero_heap) {
                self.buffer = [_]u8{0} ** (size + 1);
            } else {
                self.buffer = {};
            }
            self.storage = pin.zeroStorage(c.ove_stream_storage_t);
            self.handle = null;
            self.tracker = .{};
            if (comptime !pin.zero_heap) {
                try err.fromCode(c.ove_stream_create(&self.handle, size, trigger));
            } else {
                try err.fromCode(c.ove_stream_init(
                    &self.handle,
                    &self.storage,
                    &self.buffer,
                    size,
                    trigger,
                ));
            }
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Stream");
            if (self.handle == null) return;
            if (comptime !pin.zero_heap)
                c.ove_stream_destroy(self.handle)
            else
                c.ove_stream_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub inline fn send(self: *Self, data: []const u8, timeout_ms: u32) Error!usize {
            self.tracker.assertSame(self, "ove.Stream");
            var sent: usize = 0;
            try err.fromCode(c.ove_stream_send(self.handle, data.ptr, data.len, timeout_ms, &sent));
            return sent;
        }

        pub inline fn receive(self: *Self, buf: []u8, timeout_ms: u32) Error!usize {
            self.tracker.assertSame(self, "ove.Stream");
            var received: usize = 0;
            try err.fromCode(c.ove_stream_receive(self.handle, buf.ptr, buf.len, timeout_ms, &received));
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
