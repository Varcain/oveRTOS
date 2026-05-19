// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Event group — bitmask-based wakeups with `wait`/`waitFor`/`waitUntil`
//! and an ISR-safe `setBitsFromIsr` producer.
//!
//! Wraps `ove/eventgroup.h`. Per-op narrow error sets surface only the
//! timeout vs the substrate `Error` set; bounded waits accept a typed
//! `Duration` or `Instant`. Available when `CONFIG_OVE_EVENTGROUP` is
//! enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const time_mod = @import("time.zig");
const Duration = time_mod.Duration;
const Instant = time_mod.Instant;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

/// Bitmask type for event group bits (32 bits wide).
pub const EventBits = u32;

/// `waitBits` flag: all requested bits must be set before returning.
pub const WAIT_ALL: u32 = 0x01;
/// `waitBits` flag: clear matched bits atomically on exit.
pub const CLEAR_ON_EXIT: u32 = 0x02;

/// Error set for `EventGroup.waitBitsFor` / `waitBitsUntil`.
pub const WaitError = error{Timeout};

inline fn mapTimeoutOnly(comptime ctx: []const u8, rc: c_int) WaitError {
    return switch (rc) {
        c.OVE_ERR_TIMEOUT => error.Timeout,
        else => std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc}),
    };
}

inline fn panicOnRc(comptime ctx: []const u8, rc: c_int) void {
    if (rc < 0) std.debug.panic("ove." ++ ctx ++ ": unexpected substrate rc {d}", .{rc});
}

/// Multi-bit event group for task synchronisation.
///
/// ```zig
/// var eg = try ove.EventGroup.create(allocator);
/// defer eg.deinit();
/// _ = eg.setBits(0x03);
/// _ = eg.waitBits(0x03, ove.eventgroup.WAIT_ALL);
/// ```
pub const EventGroup = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_eventgroup_t,
    storage: *c.ove_eventgroup_storage_t,

    /// Allocate the event group's substrate-storage from `allocator`
    /// and `ove_eventgroup_init` against it.  All bits start cleared.
    pub fn create(allocator: std.mem.Allocator) Error!EventGroup {
        const storage = try allocator.create(c.ove_eventgroup_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_eventgroup_storage_t);
        var h: c.ove_eventgroup_t = null;
        try err.fromCode(c.ove_eventgroup_init(&h, storage));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: EventGroup) void {
        if (self.handle != null) c.ove_eventgroup_deinit(self.handle);
        self.allocator.destroy(self.storage);
    }

    /// Set the bits in `bits`.  Returns the bitmask after the set.
    pub inline fn setBits(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits(self.handle, bits);
    }

    /// Clear the bits in `bits`.  Returns the bitmask before the clear.
    pub inline fn clearBits(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_clear_bits(self.handle, bits);
    }

    /// Read the current bitmask without blocking.
    pub inline fn getBits(self: EventGroup) EventBits {
        return c.ove_eventgroup_get_bits(self.handle);
    }

    /// Forever-blocking wait.  Returns the matched bits.  Infallible.
    pub inline fn waitBits(self: EventGroup, bits: EventBits, flags: u32) EventBits {
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, WAIT_FOREVER, &result);
        panicOnRc("EventGroup.waitBits", rc);
        return result;
    }

    pub inline fn waitBitsFor(self: EventGroup, bits: EventBits, flags: u32, d: Duration) WaitError!EventBits {
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, d.ns, &result);
        if (rc < 0) return mapTimeoutOnly("EventGroup.waitBitsFor", rc);
        return result;
    }

    pub inline fn waitBitsUntil(self: EventGroup, bits: EventBits, flags: u32, deadline: Instant) WaitError!EventBits {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, t, &result);
        if (rc < 0) return mapTimeoutOnly("EventGroup.waitBitsUntil", rc);
        return result;
    }

    /// Set bits from an ISR.  Returns the bitmask after the set.
    pub inline fn setBitsFromIsr(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits_from_isr(self.handle, bits);
    }
};
