// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Bitmask type for event group bits (32 bits wide).
pub const EventBits = u32;

/// Flag for `waitBits()`: all requested bits must be set before returning.
///
/// When not set, `waitBits()` returns as soon as any one of the requested bits is set.
pub const WAIT_ALL: u32 = 0x01;

/// Flag for `waitBits()`: clear the matched bits atomically on exit.
///
/// When set, all bits that satisfied the wait condition are cleared before
/// `waitBits()` returns.
pub const CLEAR_ON_EXIT: u32 = 0x02;

/// Multi-bit event group for task synchronization.
///
/// An event group holds a 32-bit bitmask. Tasks can set, clear, and wait
/// on arbitrary combinations of bits. Supports both heap and zero-heap
/// backends.
pub const EventGroup = struct {
    handle: c.ove_eventgroup_t,

    /// Create and return a new event group with all bits cleared.
    ///
    /// In zero-heap mode, the storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS fails to create the event group.
    pub fn create() Error!EventGroup {
        var h: c.ove_eventgroup_t = null;
        if (comptime @hasDecl(c, "ove_eventgroup_create")) {
            try err.fromCode(c.ove_eventgroup_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_eventgroup_storage_t = std.mem.zeroes(c.ove_eventgroup_storage_t);
            };
            try err.fromCode(c.ove_eventgroup_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the event group and release underlying RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed event group.
    pub fn destroy(self: *EventGroup) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_eventgroup_destroy"))
            c.ove_eventgroup_destroy(self.handle)
        else
            c.ove_eventgroup_deinit(self.handle);
        self.handle = null;
    }

    /// Set one or more bits in the event group.
    ///
    /// Returns the value of the event bits immediately after setting.
    /// Any task waiting on those bits may be unblocked.
    pub fn setBits(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits(self.handle, bits);
    }

    /// Clear one or more bits in the event group.
    ///
    /// Returns the value of the event bits immediately after clearing.
    pub fn clearBits(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_clear_bits(self.handle, bits);
    }

    /// Read the current value of all event bits without blocking.
    pub fn getBits(self: EventGroup) EventBits {
        return c.ove_eventgroup_get_bits(self.handle);
    }

    /// Block until the specified bits are set (according to `flags`), or timeout.
    ///
    /// `bits` is the bitmask of bits to wait for.
    /// `flags` is a combination of `WAIT_ALL` and/or `CLEAR_ON_EXIT`.
    /// `timeout_ms` is the maximum wait time; use `wait_forever` to block indefinitely.
    ///
    /// Returns the value of the event bits at the moment the wait condition was satisfied.
    /// Returns `Error.Timeout` if the condition is not met within the timeout.
    pub fn waitBits(self: EventGroup, bits: EventBits, flags: u32, timeout_ms: u32) Error!EventBits {
        var result: EventBits = 0;
        try err.fromCode(c.ove_eventgroup_wait_bits(
            self.handle,
            bits,
            flags,
            timeout_ms,
            &result,
        ));
        return result;
    }

    /// Set one or more bits from an interrupt service routine.
    ///
    /// Returns the value of the event bits immediately after setting.
    /// Must only be called from ISR context.
    pub fn setBitsFromIsr(self: EventGroup, bits: EventBits) EventBits {
        return c.ove_eventgroup_set_bits_from_isr(self.handle, bits);
    }
};
