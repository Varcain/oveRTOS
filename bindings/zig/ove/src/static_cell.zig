// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! `StaticCell(T)` — an init-once container for zero-heap patterns.
//!
//! Holds a value that is constructed exactly once at startup and then shared
//! by reference. Used by the `shared!` macros in zero-heap apps where the
//! kernel object must live in BSS and survive `appMain()` returning.

const std = @import("std");

/// Init-once container for zero-heap patterns.
/// Stores a value that can be initialized exactly once and then shared.
pub fn StaticCell(comptime T: type) type {
    return struct {
        state: State = .empty,

        const Self = @This();
        const State = union(enum) {
            empty,
            initialized: T,
        };

        /// Initialize the cell with a value. Panics if already initialized.
        pub fn init(self: *Self, value: T) *T {
            std.debug.assert(self.state == .empty);
            self.state = .{ .initialized = value };
            return &self.state.initialized;
        }

        /// Get a pointer to the value, or null if not yet initialized.
        pub fn tryGet(self: *Self) ?*T {
            return switch (self.state) {
                .empty => null,
                .initialized => |*v| v,
            };
        }

        /// Get a pointer to the value.
        ///
        /// Asserts the cell has been initialized via `init()` — panics
        /// otherwise. This is intended for the init-once-at-startup
        /// invariant; if a caller cannot guarantee prior initialization,
        /// use `tryGet()` for the fallible (`?*T`) form instead.
        pub fn get(self: *Self) *T {
            return self.tryGet() orelse @panic("StaticCell not initialized");
        }
    };
}
