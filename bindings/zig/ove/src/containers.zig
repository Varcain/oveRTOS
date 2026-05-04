// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! General-purpose, fixed-capacity containers for oveRTOS Zig apps.
//!
//! Two tiers complement each other:
//!
//! 1. **Thin wrappers** — `Vec(T, N)` and `String(N)` here.  Storage is
//!    embedded inline; capacity is a comptime parameter; no allocator is
//!    ever consulted.  Mirrors the existing `Queue(T, N)` / `Stream(N)`
//!    pattern.  Use these on hot paths and where ergonomics matter.
//!
//! 2. **Stdlib + `fixedBufferAlloc`** — for hashmaps, deques, priority
//!    queues, and other complex containers, lean on
//!    `std.AutoHashMapUnmanaged`, `std.ArrayListUnmanaged`,
//!    `std.PriorityQueue`, etc., and feed them an allocator carved out of
//!    a static byte slice via `fixedBufferAlloc`.
//!
//! Both tiers compile in heap and zeroheap modes without `cfg`-style
//! gating because neither path touches the global heap.
//!
//! ## Tier 1 example
//!
//! ```zig
//! var buf = ove.Vec(u8, 64).init();
//! try buf.append(0xAB);
//! try buf.appendSlice(&[_]u8{ 1, 2, 3 });
//!
//! var name = ove.String(32).init();
//! try name.appendSlice("hello");
//! try name.format(", count={d}", .{42});
//! ```
//!
//! ## Tier 2 example
//!
//! ```zig
//! var hash_storage: [1024]u8 = undefined;
//! var fba = ove.fixedBufferAlloc(&hash_storage);
//! const allocator = fba.allocator();
//!
//! var map = std.AutoHashMapUnmanaged(u32, u32){};
//! try map.put(allocator, 1, 100);
//! ```
//!
//! ## Note on Zig 0.15
//!
//! Zig 0.15 removed `std.BoundedArray` from the standard library.  The
//! recommended replacements are the `Bounded` methods on
//! `std.ArrayListUnmanaged` (when interop with stdlib algorithms is
//! needed) or `Vec(T, N)` here (when ergonomic embedded use is the goal).

const std = @import("std");

/// The only failure these containers can produce.  `error.NoMemory` is
/// also a member of the binding's broader [`@import("error.zig").Error`]
/// set, so a `try v.append(x)` in a function returning `ove.Error!void`
/// composes without ceremony.
pub const Error = error{NoMemory};

/// Fixed-capacity vector with embedded storage.
///
/// Capacity `N` is a comptime parameter; the backing `[N]T` array lives
/// inline in the struct.  No allocator is consulted, so the same code runs
/// in heap and zeroheap modes.
///
/// All push-shaped methods return `Error.NoMemory` when the container is
/// full — consistent with the rest of the binding's error type.
///
/// ```zig
/// var v = ove.Vec(u32, 4).init();
/// try v.append(1);
/// try v.append(2);
/// const last = v.pop().?;     // 2
/// for (v.constSlice()) |x| { _ = x; }
/// ```
pub fn Vec(comptime T: type, comptime N: usize) type {
    return struct {
        const Self = @This();

        items: [N]T = undefined,
        len: usize = 0,

        /// Construct an empty vector.  Cheap — does not zero the backing
        /// storage; reads past `len` are UB and prevented by the API.
        pub fn init() Self {
            return .{ .items = undefined, .len = 0 };
        }

        /// Append `item`.  Returns `Error.NoMemory` if the vector is full.
        pub inline fn append(self: *Self, item: T) Error!void {
            if (self.len >= N) return Error.NoMemory;
            self.items[self.len] = item;
            self.len += 1;
        }

        /// Append every element of `s`.  Returns `Error.NoMemory` if there
        /// isn't room for all of them; on failure the vector is left
        /// unchanged.
        pub fn appendSlice(self: *Self, s: []const T) Error!void {
            if (self.len + s.len > N) return Error.NoMemory;
            @memcpy(self.items[self.len..][0..s.len], s);
            self.len += s.len;
        }

        /// Remove and return the last element, or `null` if empty.
        pub fn pop(self: *Self) ?T {
            if (self.len == 0) return null;
            self.len -= 1;
            return self.items[self.len];
        }

        /// Reset the length to zero.  Does not zero the backing storage.
        pub fn clear(self: *Self) void {
            self.len = 0;
        }

        /// Mutable view of the live elements.
        pub fn slice(self: *Self) []T {
            return self.items[0..self.len];
        }

        /// Read-only view of the live elements.
        pub fn constSlice(self: *const Self) []const T {
            return self.items[0..self.len];
        }

        /// Compile-time capacity.
        pub fn capacity() usize {
            return N;
        }

        /// Is the vector at capacity?
        pub fn isFull(self: *const Self) bool {
            return self.len >= N;
        }

        /// Is the vector empty?
        pub fn isEmpty(self: *const Self) bool {
            return self.len == 0;
        }
    };
}

/// Fixed-capacity, UTF-8-friendly string with embedded byte storage.
///
/// Capacity `N` is the byte capacity (not codepoint count).  Methods
/// return `Error.NoMemory` when there isn't room.  No allocator is
/// consulted.
///
/// ```zig
/// var s = ove.String(64).init();
/// try s.appendSlice("count=");
/// try s.format("{d}", .{42});
/// std.debug.print("{s}\n", .{s.slice()});
/// ```
pub fn String(comptime N: usize) type {
    return struct {
        const Self = @This();

        bytes: [N]u8 = undefined,
        len: usize = 0,

        /// Construct an empty string.
        pub fn init() Self {
            return .{ .bytes = undefined, .len = 0 };
        }

        /// Append a single byte.  Returns `Error.NoMemory` if full.
        pub inline fn appendByte(self: *Self, b: u8) Error!void {
            if (self.len >= N) return Error.NoMemory;
            self.bytes[self.len] = b;
            self.len += 1;
        }

        /// Append a byte slice.  Returns `Error.NoMemory` if there isn't
        /// room for all of it; on failure the string is left unchanged.
        pub fn appendSlice(self: *Self, s: []const u8) Error!void {
            if (self.len + s.len > N) return Error.NoMemory;
            @memcpy(self.bytes[self.len..][0..s.len], s);
            self.len += s.len;
        }

        /// Format-append using `std.fmt.bufPrint`.  Returns
        /// `Error.NoMemory` if the formatted output doesn't fit in the
        /// remaining capacity; on failure the string is left unchanged.
        pub fn format(self: *Self, comptime fmt: []const u8, args: anytype) Error!void {
            const remaining = self.bytes[self.len..];
            const written = std.fmt.bufPrint(remaining, fmt, args) catch
                return Error.NoMemory;
            self.len += written.len;
        }

        /// Read-only view of the live bytes.
        pub fn slice(self: *const Self) []const u8 {
            return self.bytes[0..self.len];
        }

        /// Append a null terminator and return a `[*:0]const u8` pointer
        /// suitable for handing to a C API.  Returns `Error.NoMemory` if
        /// the string is at capacity.  Subsequent mutations invalidate
        /// the returned pointer.
        pub fn cStr(self: *Self) Error![*:0]const u8 {
            if (self.len >= N) return Error.NoMemory;
            self.bytes[self.len] = 0;
            return @ptrCast(&self.bytes);
        }

        /// Reset the length to zero.
        pub fn clear(self: *Self) void {
            self.len = 0;
        }

        /// Compile-time capacity in bytes.
        pub fn capacity() usize {
            return N;
        }

        /// Is the buffer at capacity?
        pub fn isFull(self: *const Self) bool {
            return self.len >= N;
        }

        /// Is the buffer empty?
        pub fn isEmpty(self: *const Self) bool {
            return self.len == 0;
        }
    };
}

/// Construct a `FixedBufferAllocator` over `buffer`.
///
/// Equivalent to `std.heap.FixedBufferAllocator.init(buffer)`; provided
/// here as the project-blessed entry point for "use stdlib unmanaged
/// containers without ever touching the heap."
///
/// The returned struct holds a cursor — the caller must keep it alive for
/// as long as any container backed by `.allocator()` is in use.
///
/// ```zig
/// var storage: [1024]u8 = undefined;
/// var fba = ove.fixedBufferAlloc(&storage);
/// var list = std.ArrayListUnmanaged(u32){};
/// try list.append(fba.allocator(), 42);
/// ```
pub fn fixedBufferAlloc(buffer: []u8) std.heap.FixedBufferAllocator {
    return std.heap.FixedBufferAllocator.init(buffer);
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

const testing = std.testing;

test "Vec basic append / pop / slice" {
    var v = Vec(u32, 4).init();
    try testing.expect(v.isEmpty());
    try v.append(1);
    try v.append(2);
    try v.append(3);
    try testing.expectEqual(@as(usize, 3), v.len);
    try testing.expectEqualSlices(u32, &[_]u32{ 1, 2, 3 }, v.constSlice());
    try testing.expectEqual(@as(?u32, 3), v.pop());
    try testing.expectEqual(@as(usize, 2), v.len);
}

test "Vec capacity overflow returns NoMemory" {
    var v = Vec(u8, 2).init();
    try v.append(1);
    try v.append(2);
    try testing.expectError(Error.NoMemory, v.append(3));
    try testing.expectEqual(@as(usize, 2), v.len);
}

test "Vec appendSlice succeeds and rejects" {
    var v = Vec(u8, 4).init();
    try v.appendSlice(&[_]u8{ 1, 2 });
    try testing.expectError(Error.NoMemory, v.appendSlice(&[_]u8{ 3, 4, 5 }));
    try testing.expectEqual(@as(usize, 2), v.len);
    try v.appendSlice(&[_]u8{ 3, 4 });
    try testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3, 4 }, v.constSlice());
}

test "String append, slice, format" {
    var s = String(32).init();
    try s.appendSlice("count=");
    try s.format("{d}", .{42});
    try testing.expectEqualStrings("count=42", s.slice());
    try s.appendByte('!');
    try testing.expectEqualStrings("count=42!", s.slice());
}

test "String overflow leaves buffer unchanged" {
    var s = String(8).init();
    try s.appendSlice("hello");
    try testing.expectError(Error.NoMemory, s.appendSlice(", world"));
    try testing.expectEqualStrings("hello", s.slice());
    try testing.expectError(Error.NoMemory, s.format(", {s}", .{"world"}));
    try testing.expectEqualStrings("hello", s.slice());
}

test "String cStr appends null and returns pointer" {
    var s = String(16).init();
    try s.appendSlice("hi");
    const c = try s.cStr();
    try testing.expectEqual(@as(u8, 'h'), c[0]);
    try testing.expectEqual(@as(u8, 'i'), c[1]);
    try testing.expectEqual(@as(u8, 0), c[2]);
}

test "String cStr fails when at capacity" {
    var s = String(4).init();
    try s.appendSlice("abcd");
    try testing.expectError(Error.NoMemory, s.cStr());
}

test "fixedBufferAlloc backs std.ArrayListUnmanaged" {
    var storage: [256]u8 = undefined;
    var fba = fixedBufferAlloc(&storage);
    const allocator = fba.allocator();
    var list: std.ArrayListUnmanaged(u32) = .{};
    try list.append(allocator, 1);
    try list.append(allocator, 2);
    try list.append(allocator, 3);
    try testing.expectEqualSlices(u32, &[_]u32{ 1, 2, 3 }, list.items);
}
