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

    pub inline fn waitBits(self: EventGroup, bits: EventBits, flags: u32, timeout_ms: u32) Error!EventBits {
        var result: EventBits = 0;
        try err.fromCode(c.ove_eventgroup_wait_bits(self.handle, bits, flags, timeout_ms, &result));
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

    pub inline fn waitBits(self: *EventGroup, bits: EventBits, flags: u32, timeout_ms: u32) Error!EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        var result: EventBits = 0;
        try err.fromCode(c.ove_eventgroup_wait_bits(self.handle, bits, flags, timeout_ms, &result));
        return result;
    }

    /// ISR-safe — skips pin check (panics from ISR are unsafe; init must
    /// have completed in task context before any ISR fires).
    pub inline fn setBitsFromIsr(self: *EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits_from_isr(self.handle, bits);
    }
};
