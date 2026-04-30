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

/// TLS session.
///
/// ```zig
/// var tls: ove.TlsSession = undefined;
/// try tls.init();
/// defer tls.deinit();
/// ```
pub const Session = struct {
    storage: pin.Storage(c.ove_tls_storage_t),
    handle: c.ove_tls_t,
    tracker: pin.Tracker,

    pub fn init(self: *Session) Error!void {
        self.storage = pin.zeroStorage(c.ove_tls_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_tls_create(&self.handle));
        } else {
            try err.fromCode(c.ove_tls_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Session) void {
        self.tracker.assertSame(self, "ove.TlsSession");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_tls_destroy(self.handle)
        else
            c.ove_tls_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }
};
