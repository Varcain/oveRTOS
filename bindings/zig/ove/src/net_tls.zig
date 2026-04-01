// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! TLS/SSL session wrapper (mbedTLS).
//!
//! Provides encrypted communication over an established TCP socket.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const TcpStream = @import("net.zig").TcpStream;

/// TLS session configuration.
pub const TlsConfig = struct {
    /// PEM or DER CA certificate (null to skip verification).
    ca_cert: ?[]const u8 = null,
    /// Expected server hostname for SNI (null to skip).
    hostname: ?[:0]const u8 = null,
};

/// TLS session with RAII cleanup.
pub const Session = struct {
    handle: c.ove_tls_t,

    /// Create a TLS session.
    pub fn create() Error!Session {
        var h: c.ove_tls_t = null;
        if (comptime @hasDecl(c, "ove_tls_create")) {
            try err.fromCode(c.ove_tls_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_tls_storage_t = std.mem.zeroes(c.ove_tls_storage_t);
            };
            try err.fromCode(c.ove_tls_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the TLS session.
    pub fn destroy(self: *Session) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_tls_destroy"))
            c.ove_tls_destroy(self.handle)
        else
            c.ove_tls_deinit(self.handle);
        self.handle = null;
    }

    /// Perform TLS handshake over an established TCP connection.
    pub fn handshake(self: Session, sock: TcpStream, cfg: TlsConfig) Error!void {
        var cc: c.ove_tls_config_t = std.mem.zeroes(c.ove_tls_config_t);
        if (cfg.ca_cert) |cert| {
            cc.ca_cert = cert.ptr;
            cc.ca_cert_len = cert.len;
        }
        if (cfg.hostname) |host| {
            cc.hostname = host.ptr;
        }
        try err.fromCode(c.ove_tls_handshake(self.handle, sock.handle, &cc));
    }

    /// Send data over the encrypted session. Returns bytes sent.
    pub fn send(self: Session, data: []const u8) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_tls_send(self.handle, data.ptr, data.len, &sent));
        return sent;
    }

    /// Receive data from the encrypted session. Returns bytes received.
    pub fn recv(self: Session, buf: []u8) Error!usize {
        var received: usize = 0;
        try err.fromCode(c.ove_tls_recv(self.handle, buf.ptr, buf.len, &received));
        return received;
    }

    /// Shut down the TLS session (sends close_notify). Socket is NOT closed.
    pub fn close(self: Session) void {
        c.ove_tls_close(self.handle);
    }
};
