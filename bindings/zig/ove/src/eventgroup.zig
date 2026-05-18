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
const time_mod = @import("time.zig");
const Duration = time_mod.Duration;
const Instant = time_mod.Instant;
const WAIT_FOREVER = c.OVE_WAIT_FOREVER;

// Per-operation narrow error set.
/// Error set for `EventGroup.waitBitsFor`.  `waitBits` is forever-
/// blocking and infallible after a successful create/init.
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

/// Bitmask type for event group bits (32 bits wide).
pub const EventBits = u32;

/// `waitBits` flag: all requested bits must be set before returning.
pub const WAIT_ALL: u32 = 0x01;
/// `waitBits` flag: clear matched bits atomically on exit.
pub const CLEAR_ON_EXIT: u32 = 0x02;

/// Multi-bit event group for task synchronisation.
///
/// Heap mode (value-returning create):
///
/// ```zig
/// var eg = try ove.EventGroup.create();
/// defer eg.deinit();
/// _ = eg.setBits(0x03);
/// _ = try eg.waitBits(0x03, ove.eventgroup.WAIT_ALL, ove.wait_forever);
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var eg: ove.EventGroup = undefined;
/// try eg.init();
/// defer eg.deinit();
/// ```
pub const EventGroup = if (pin.zero_heap) ZeroHeapEventGroup else HeapEventGroup;

const HeapEventGroup = struct {
    handle: c.ove_eventgroup_t,

    pub fn create() Error!EventGroup {
        var h: c.ove_eventgroup_t = null;
        try err.fromCode(c.ove_eventgroup_create(&h));
        return .{ .handle = h };
    }

    pub fn deinit(self: EventGroup) void {
        if (self.handle == null) return;
        c.ove_eventgroup_destroy(self.handle);
    }

    pub inline fn setBits(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits(self.handle, bits);
    }

    pub inline fn clearBits(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_clear_bits(self.handle, bits);
    }

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

    /// Bounded-duration wait.
    pub inline fn waitBitsFor(self: EventGroup, bits: EventBits, flags: u32, d: Duration) WaitError!EventBits {
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, d.ns, &result);
        if (rc < 0) return mapTimeoutOnly("EventGroup.waitBitsFor", rc);
        return result;
    }

    /// Deadline-based wait.
    pub inline fn waitBitsUntil(self: EventGroup, bits: EventBits, flags: u32, deadline: Instant) WaitError!EventBits {
        const t = time_mod.deadlineToTimeoutNs(deadline);
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, t, &result);
        if (rc < 0) return mapTimeoutOnly("EventGroup.waitBitsUntil", rc);
        return result;
    }

    pub inline fn setBitsFromIsr(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits_from_isr(self.handle, bits);
    }
};

const ZeroHeapEventGroup = struct {
    storage: c.ove_eventgroup_storage_t,
    handle: c.ove_eventgroup_t,
    tracker: pin.Tracker,

    pub fn init(self: *EventGroup) Error!void {
        self.storage = std.mem.zeroes(c.ove_eventgroup_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_eventgroup_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *EventGroup) void {
        self.tracker.assertSame(self, "ove.EventGroup");
        if (self.handle == null) return;
        c.ove_eventgroup_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub inline fn setBits(self: *EventGroup, bits: EventBits) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        return c.ove_eventgroup_set_bits(self.handle, bits);
    }

    pub inline fn clearBits(self: *EventGroup, bits: EventBits) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        return c.ove_eventgroup_clear_bits(self.handle, bits);
    }

    pub inline fn getBits(self: *EventGroup) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        return c.ove_eventgroup_get_bits(self.handle);
    }

    pub inline fn waitBits(self: *EventGroup, bits: EventBits, flags: u32) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, WAIT_FOREVER, &result);
        panicOnRc("EventGroup.waitBits", rc);
        return result;
    }

    pub inline fn waitBitsFor(self: *EventGroup, bits: EventBits, flags: u32, d: Duration) WaitError!EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, d.ns, &result);
        if (rc < 0) return mapTimeoutOnly("EventGroup.waitBitsFor", rc);
        return result;
    }

    pub inline fn waitBitsUntil(self: *EventGroup, bits: EventBits, flags: u32, deadline: Instant) WaitError!EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        const t = time_mod.deadlineToTimeoutNs(deadline);
        var result: EventBits = 0;
        const rc = c.ove_eventgroup_wait_bits(self.handle, bits, flags, t, &result);
        if (rc < 0) return mapTimeoutOnly("EventGroup.waitBitsUntil", rc);
        return result;
    }

    /// ISR-safe — skips pin check (panics from ISR are unsafe; init must
    /// have completed in task context before any ISR fires).
    pub inline fn setBitsFromIsr(self: *EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits_from_isr(self.handle, bits);
    }
};
