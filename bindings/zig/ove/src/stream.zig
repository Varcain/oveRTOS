// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Variable-length byte stream buffer for inter-task data transfer.
///
/// Unlike a message queue, a stream buffer transfers arbitrary-length byte
/// sequences without fixed message boundaries. A `trigger` level controls
/// the minimum number of bytes that must be available before a blocked
/// receiver is unbloken. Supports both heap and zero-heap backends.
pub const Stream = struct {
    handle: c.ove_stream_t,

    /// Create a stream buffer with a capacity of `size` bytes and a trigger level.
    ///
    /// `trigger` is the minimum number of bytes a receiver waits for before being
    /// unblocked (0 or 1 means "wake on any byte"). In zero-heap mode, the internal
    /// storage and ring buffer are comptime-unique static variables.
    /// Returns `Error` if the RTOS fails to create the stream buffer.
    pub fn create(comptime size: usize, trigger: usize) Error!Stream {
        var h: c.ove_stream_t = null;
        if (comptime @hasDecl(c, "ove_stream_create")) {
            try err.fromCode(c.ove_stream_create(&h, size, trigger));
        } else {
            const S = struct {
                var storage: c.ove_stream_storage_t = std.mem.zeroes(c.ove_stream_storage_t);
                var buffer: [size + 1]u8 = [_]u8{0} ** (size + 1);
            };
            try err.fromCode(c.ove_stream_init(&h, &S.storage, &S.buffer, size, trigger));
        }
        return .{ .handle = h };
    }

    /// Destroy the stream buffer and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed stream.
    pub fn destroy(self: *Stream) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_stream_destroy"))
            c.ove_stream_destroy(self.handle)
        else
            c.ove_stream_deinit(self.handle);
        self.handle = null;
    }

    /// Write `data` into the stream, blocking up to `timeout_ms` milliseconds.
    ///
    /// Returns the number of bytes actually written on success. If the stream
    /// has insufficient space and `timeout_ms` expires, returns `Error.Timeout`.
    pub fn send(self: Stream, data: []const u8, timeout_ms: u32) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_stream_send(
            self.handle,
            data.ptr,
            data.len,
            timeout_ms,
            &sent,
        ));
        return sent;
    }

    /// Read up to `buf.len` bytes from the stream, blocking up to `timeout_ms`.
    ///
    /// Returns the number of bytes actually read. Returns `Error.Timeout` if
    /// fewer than `trigger` bytes are available before the timeout expires.
    pub fn receive(self: Stream, buf: []u8, timeout_ms: u32) Error!usize {
        var received: usize = 0;
        try err.fromCode(c.ove_stream_receive(
            self.handle,
            buf.ptr,
            buf.len,
            timeout_ms,
            &received,
        ));
        return received;
    }

    /// Write `data` into the stream from an interrupt service routine (non-blocking).
    ///
    /// Returns the number of bytes written, which may be less than `data.len`
    /// if the stream is full. Must only be called from ISR context.
    pub fn sendFromIsr(self: Stream, data: []const u8) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_stream_send_from_isr(
            self.handle,
            data.ptr,
            data.len,
            &sent,
        ));
        return sent;
    }

    /// Read bytes from the stream in an interrupt service routine (non-blocking).
    ///
    /// Returns the number of bytes read into `buf`. Must only be called from ISR context.
    pub fn receiveFromIsr(self: Stream, buf: []u8) Error!usize {
        var received: usize = 0;
        try err.fromCode(c.ove_stream_receive_from_isr(
            self.handle,
            buf.ptr,
            buf.len,
            &received,
        ));
        return received;
    }

    /// Discard all data in the stream buffer, resetting it to empty.
    ///
    /// Returns `Error` if the reset fails (e.g. a task is currently blocked on it).
    pub fn reset(self: Stream) Error!void {
        try err.fromCode(c.ove_stream_reset(self.handle));
    }

    /// Return the number of bytes currently available to read from the stream.
    pub fn bytesAvailable(self: Stream) usize {
        return c.ove_stream_bytes_available(self.handle);
    }
};
