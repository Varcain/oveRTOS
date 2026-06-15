// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Byte-stream ring buffer — `Stream(N)` with `send`/`recv` and bounded-wait
//! variants.
//!
//! Wraps `ove/stream.h`. Unlike `Queue`, streams transport raw bytes (not
//! typed items) and surface a `trigger` threshold at construction time: a
//! receiver wakes only after at least `trigger` bytes are available.
//! Available when `CONFIG_OVE_STREAM` is enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const time_mod = @import("time.zig");
const Duration = time_mod.Duration;
const Instant = time_mod.Instant;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

/// Error set for `Stream.sendFor` / `sendUntil` / `sendFromIsr`.
pub const SendError = error{Timeout};
/// Error set for `Stream.recvFor` / `recvUntil` / `receiveFromIsr`.
pub const RecvError = error{Timeout};

inline fn mapTimeoutOnly(comptime ctx: []const u8, rc: c_int) error{Timeout} {
    return switch (rc) {
        c.OVE_ERR_TIMEOUT => error.Timeout,
        else => std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc}),
    };
}

inline fn panicOnRc(comptime ctx: []const u8, rc: c_int) void {
    if (rc < 0) std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc});
}

/// Variable-length byte stream buffer.
///
/// ```zig
/// var s = try ove.Stream(256).create(allocator, 1);  // trigger=1
/// defer s.deinit();
/// _ = s.send("hello");
/// ```
pub fn Stream(comptime size: usize) type {
    return struct {
        const Self = @This();

        const Backing = struct {
            buffer: [size + 1]u8,
            storage: c.ove_stream_storage_t,
        };

        allocator: std.mem.Allocator,
        handle: c.ove_stream_t,
        backing: ?*Backing,

        /// Allocate the stream's ring buffer + substrate-storage from
        /// `allocator` and `ove_stream_init` against it.  `trigger` is
        /// the minimum bytes a receiver waits for before unblocking
        /// (0 or 1 = wake on any byte).
        pub fn create(allocator: std.mem.Allocator, trigger: usize) Error!Self {
            const backing = try allocator.create(Backing);
            errdefer allocator.destroy(backing);
            backing.buffer = std.mem.zeroes([size + 1]u8);
            backing.storage = std.mem.zeroes(c.ove_stream_storage_t);
            var h: c.ove_stream_t = null;
            try err.fromCode(c.ove_stream_init(&h, &backing.storage, &backing.buffer, size, trigger));
            return .{ .allocator = allocator, .handle = h, .backing = backing };
        }

        /// Idempotent — clears `handle` and `backing` after teardown so a
        /// redundant `defer s.deinit()` after an explicit `deinit()` is a
        /// safe no-op rather than a double free.
        pub fn deinit(self: *Self) void {
            if (self.handle) |h| {
                c.ove_stream_deinit(h);
                self.handle = null;
            }
            if (self.backing) |b| {
                self.allocator.destroy(b);
                self.backing = null;
            }
        }

        /// Forever-blocking send.  Returns the number of bytes actually
        /// written (always equal to `data.len` on the WAIT_FOREVER path —
        /// the substrate only short-writes under bounded timeouts).
        pub inline fn send(self: Self, data: []const u8) usize {
            var sent: usize = 0;
            const rc = c.ove_stream_send(self.handle, data.ptr, data.len, WAIT_FOREVER, &sent);
            panicOnRc("Stream.send", rc);
            return sent;
        }

        pub inline fn sendFor(self: Self, data: []const u8, d: Duration) SendError!usize {
            var sent: usize = 0;
            const rc = c.ove_stream_send(self.handle, data.ptr, data.len, d.ns, &sent);
            if (rc < 0) return mapTimeoutOnly("Stream.sendFor", rc);
            return sent;
        }

        pub inline fn sendUntil(self: Self, data: []const u8, deadline: Instant) SendError!usize {
            const t = time_mod.deadlineToTimeoutNs(deadline);
            var sent: usize = 0;
            const rc = c.ove_stream_send(self.handle, data.ptr, data.len, t, &sent);
            if (rc < 0) return mapTimeoutOnly("Stream.sendUntil", rc);
            return sent;
        }

        /// Forever-blocking receive.  Returns the number of bytes
        /// actually read (≥ the stream's `trigger` setting — the
        /// substrate only wakes once at least that many are available).
        pub inline fn recv(self: Self, buf: []u8) usize {
            var received: usize = 0;
            const rc = c.ove_stream_receive(self.handle, buf.ptr, buf.len, WAIT_FOREVER, &received);
            panicOnRc("Stream.recv", rc);
            return received;
        }

        pub inline fn recvFor(self: Self, buf: []u8, d: Duration) RecvError!usize {
            var received: usize = 0;
            const rc = c.ove_stream_receive(self.handle, buf.ptr, buf.len, d.ns, &received);
            if (rc < 0) return mapTimeoutOnly("Stream.recvFor", rc);
            return received;
        }

        pub inline fn recvUntil(self: Self, buf: []u8, deadline: Instant) RecvError!usize {
            const t = time_mod.deadlineToTimeoutNs(deadline);
            var received: usize = 0;
            const rc = c.ove_stream_receive(self.handle, buf.ptr, buf.len, t, &received);
            if (rc < 0) return mapTimeoutOnly("Stream.recvUntil", rc);
            return received;
        }

        /// Push bytes from an ISR.  Non-blocking; returns actually-
        /// written count (may be 0 if the ring is full).
        pub fn sendFromIsr(self: Self, data: []const u8) SendError!usize {
            var sent: usize = 0;
            const rc = c.ove_stream_send_from_isr(self.handle, data.ptr, data.len, &sent);
            if (rc < 0) return mapTimeoutOnly("Stream.sendFromIsr", rc);
            return sent;
        }

        /// Pull bytes from an ISR.  Non-blocking; returns actually-
        /// read count (may be 0 if the ring is empty).
        pub fn receiveFromIsr(self: Self, buf: []u8) RecvError!usize {
            var received: usize = 0;
            const rc = c.ove_stream_receive_from_isr(self.handle, buf.ptr, buf.len, &received);
            if (rc < 0) return mapTimeoutOnly("Stream.receiveFromIsr", rc);
            return received;
        }

        pub fn reset(self: Self) Error!void {
            try err.fromCode(c.ove_stream_reset(self.handle));
        }

        pub fn bytesAvailable(self: Self) usize {
            return c.ove_stream_bytes_available(self.handle);
        }

        // ----- std.io.GenericReader / GenericWriter integration -----
        //
        // `recv` / `send` are forever-blocking and infallible (panic on
        // substrate programming-bug codes), so the `Reader.Error` /
        // `Writer.Error` set is the empty `error{}` — every `std.io`
        // consumer (`bufferedReader`, line parsers, codecs) composes
        // with `Stream(N)` without an outer error union.

        const StreamReader = std.io.GenericReader(*const Self, error{}, struct {
            fn read(self: *const Self, buf: []u8) error{}!usize {
                return self.recv(buf);
            }
        }.read);

        const StreamWriter = std.io.GenericWriter(*const Self, error{}, struct {
            fn write(self: *const Self, buf: []const u8) error{}!usize {
                return self.send(buf);
            }
        }.write);

        /// `std.io.GenericReader` view of this stream — forever-blocking
        /// `recv` is wired as the read backend.
        pub fn reader(self: *const Self) StreamReader {
            return .{ .context = self };
        }

        /// `std.io.GenericWriter` view of this stream — forever-blocking
        /// `send` is wired as the write backend.
        pub fn writer(self: *const Self) StreamWriter {
            return .{ .context = self };
        }
    };
}
