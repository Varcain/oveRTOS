// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! TLS session wrapper over mbedTLS — handshake, send, recv, and close on top
//! of a connected `ove_socket_t`.
//!
//! Wraps `ove/net_tls.h`. The public `Session` type is gated on
//! `pin.zero_heap` (`HeapSession` vs `ZeroHeapSession`). Available when
//! `CONFIG_OVE_NET_TLS` is enabled.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

/// TLS session.
///
/// Heap mode (value-returning):
///
/// ```zig
/// var tls = try ove.net_tls.Session.create();
/// defer tls.deinit();
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var tls: ove.net_tls.Session = undefined;
/// try tls.init();
/// defer tls.deinit();
/// ```
pub const Session = if (pin.zero_heap) ZeroHeapSession else HeapSession;

const HeapSession = struct {
    handle: c.ove_tls_t,

    pub fn create() Error!Session {
        var h: c.ove_tls_t = null;
        try err.fromCode(c.ove_tls_create(&h));
        return .{ .handle = h };
    }

    pub fn deinit(self: *Session) void {
        if (self.handle == null) return;
        c.ove_tls_destroy(self.handle);
        self.handle = null;
    }
};

const ZeroHeapSession = struct {
    storage: c.ove_tls_storage_t,
    handle: c.ove_tls_t,
    tracker: pin.Tracker,

    pub fn init(self: *Session) Error!void {
        self.storage = std.mem.zeroes(c.ove_tls_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_tls_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Session) void {
        self.tracker.assertSame(self, "ove.TlsSession");
        if (self.handle == null) return;
        c.ove_tls_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }
};
