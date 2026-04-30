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
/// ```zig
/// var eg: ove.EventGroup = undefined;
/// try eg.init();
/// defer eg.deinit();
/// _ = eg.setBits(0x03);
/// _ = try eg.waitBits(0x03, ove.eventgroup.WAIT_ALL, ove.wait_forever);
/// ```
pub const EventGroup = struct {
    storage: pin.Storage(c.ove_eventgroup_storage_t),
    handle: c.ove_eventgroup_t,
    tracker: pin.Tracker,

    pub fn init(self: *EventGroup) Error!void {
        self.storage = pin.zeroStorage(c.ove_eventgroup_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_eventgroup_create(&self.handle));
        } else {
            try err.fromCode(c.ove_eventgroup_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *EventGroup) void {
        self.tracker.assertSame(self, "ove.EventGroup");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_eventgroup_destroy(self.handle)
        else
            c.ove_eventgroup_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn setBits(self: *EventGroup, bits: EventBits) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        return c.ove_eventgroup_set_bits(self.handle, bits);
    }

    pub fn clearBits(self: *EventGroup, bits: EventBits) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        return c.ove_eventgroup_clear_bits(self.handle, bits);
    }

    pub fn getBits(self: *EventGroup) EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        return c.ove_eventgroup_get_bits(self.handle);
    }

    pub fn waitBits(self: *EventGroup, bits: EventBits, flags: u32, timeout_ms: u32) Error!EventBits {
        self.tracker.assertSame(self, "ove.EventGroup");
        var result: EventBits = 0;
        try err.fromCode(c.ove_eventgroup_wait_bits(self.handle, bits, flags, timeout_ms, &result));
        return result;
    }

    /// ISR-safe — skips pin check (panics from ISR are unsafe; init must
    /// have completed in task context before any ISR fires).
    pub fn setBitsFromIsr(self: *EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits_from_isr(self.handle, bits);
    }
};
