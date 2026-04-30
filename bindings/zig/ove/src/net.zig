// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Networking primitives for oveRTOS.
//!
//! Provides typed wrappers for BSD-like sockets (TCP and UDP), network
//! interface management, DNS resolution, and socket addresses.  Resource
//! types follow the embedded-storage pattern — declare with `undefined`,
//! call `init()`, register `defer deinit()`.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

// ---------------------------------------------------------------------------
// Address  (value type, no kernel resource)
// ---------------------------------------------------------------------------

/// Generic socket address (IPv4 or IPv6).  Value type — copyable, small.
pub const Address = struct {
    inner: c.ove_sockaddr_t,

    pub fn ipv4(a: u8, b: u8, addr_c: u8, d: u8, p: u16) Address {
        var addr: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        c.ove_sockaddr_ipv4(&addr, a, b, addr_c, d, p);
        return .{ .inner = addr };
    }

    pub fn any(p: u16) Address {
        return ipv4(0, 0, 0, 0, p);
    }

    pub fn port(self: Address) u16 {
        return self.inner.port;
    }

    pub fn withPort(self: Address, p: u16) Address {
        var copy = self;
        copy.inner.port = p;
        return copy;
    }

    pub fn octets(self: Address) [4]u8 {
        return .{ self.inner.addr[0], self.inner.addr[1], self.inner.addr[2], self.inner.addr[3] };
    }
};

// ---------------------------------------------------------------------------
// NetIfConfig  (value type)
// ---------------------------------------------------------------------------

/// Network interface configuration with immutable builder pattern.
pub const NetIfConfig = struct {
    inner: c.ove_netif_config_t,

    pub fn init() NetIfConfig {
        return .{ .inner = std.mem.zeroes(c.ove_netif_config_t) };
    }

    pub fn dhcp(self: NetIfConfig) NetIfConfig {
        var copy = self;
        copy.inner.use_dhcp = 1;
        return copy;
    }

    pub fn staticIp(self: NetIfConfig, ip: Address, mask: Address, gw: Address) NetIfConfig {
        var copy = self;
        copy.inner.use_dhcp = 0;
        copy.inner.static_ip = ip.inner;
        copy.inner.netmask = mask.inner;
        copy.inner.gateway = gw.inner;
        return copy;
    }

    pub fn dns(self: NetIfConfig, addr: Address) NetIfConfig {
        var copy = self;
        copy.inner.dns = addr.inner;
        return copy;
    }
};

// ---------------------------------------------------------------------------
// NetIf
// ---------------------------------------------------------------------------

/// Network interface handle.
///
/// ```zig
/// var nif: ove.NetIf = undefined;
/// try nif.init();
/// defer nif.deinit();
/// try nif.up(ove.NetIfConfig.init().dhcp());
/// ```
pub const NetIf = struct {
    storage: pin.Storage(c.ove_netif_storage_t),
    handle: c.ove_netif_t,
    tracker: pin.Tracker,

    pub fn init(self: *NetIf) Error!void {
        self.storage = pin.zeroStorage(c.ove_netif_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_netif_create(&self.handle));
        } else {
            try err.fromCode(c.ove_netif_init(&self.handle, &self.storage));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *NetIf) void {
        self.tracker.assertSame(self, "ove.NetIf");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_netif_destroy(self.handle)
        else
            c.ove_netif_deinit(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn up(self: *NetIf, cfg: NetIfConfig) Error!void {
        self.tracker.assertSame(self, "ove.NetIf");
        try err.fromCode(c.ove_netif_up(self.handle, &cfg.inner));
    }

    pub fn down(self: *NetIf) void {
        self.tracker.assertSame(self, "ove.NetIf");
        c.ove_netif_down(self.handle);
    }

    pub const AddrInfo = struct { ip: Address, gateway: Address, netmask: Address };

    pub fn getAddr(self: *NetIf) Error!AddrInfo {
        self.tracker.assertSame(self, "ove.NetIf");
        var ip = std.mem.zeroes(c.ove_sockaddr_t);
        var gw = std.mem.zeroes(c.ove_sockaddr_t);
        var nm = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_netif_get_addr(self.handle, &ip, &gw, &nm));
        return .{ .ip = .{ .inner = ip }, .gateway = .{ .inner = gw }, .netmask = .{ .inner = nm } };
    }
};

// ---------------------------------------------------------------------------
// TcpStream
// ---------------------------------------------------------------------------

/// TCP stream socket.
///
/// ```zig
/// var sock: ove.TcpStream = undefined;
/// try sock.init();
/// defer sock.deinit();
/// try sock.connect(addr, timeout);
/// ```
pub const TcpStream = struct {
    storage: pin.Storage(c.ove_socket_storage_t),
    handle: c.ove_socket_t,
    tracker: pin.Tracker,

    pub fn init(self: *TcpStream) Error!void {
        self.storage = pin.zeroStorage(c.ove_socket_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_socket_create(&self.handle, c.OVE_AF_INET, c.OVE_SOCK_STREAM));
        } else {
            try err.fromCode(c.ove_socket_open(&self.handle, &self.storage, c.OVE_AF_INET, c.OVE_SOCK_STREAM));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *TcpStream) void {
        self.tracker.assertSame(self, "ove.TcpStream");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_socket_destroy(self.handle)
        else
            c.ove_socket_close(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn connect(self: *TcpStream, addr: Address, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.TcpStream");
        try err.fromCode(c.ove_socket_connect(self.handle, &addr.inner, timeout_ms));
    }

    pub fn send(self: *TcpStream, data: []const u8) Error!usize {
        self.tracker.assertSame(self, "ove.TcpStream");
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_send(self.handle, data.ptr, data.len, &sent));
        return sent;
    }

    pub fn recv(self: *TcpStream, buf: []u8, timeout_ms: u32) Error!usize {
        self.tracker.assertSame(self, "ove.TcpStream");
        var received: usize = 0;
        try err.fromCode(c.ove_socket_recv(self.handle, buf.ptr, buf.len, &received, timeout_ms));
        return received;
    }

    pub fn bind(self: *TcpStream, addr: Address) Error!void {
        self.tracker.assertSame(self, "ove.TcpStream");
        try err.fromCode(c.ove_socket_bind(self.handle, &addr.inner));
    }

    pub fn listen(self: *TcpStream, backlog: i32) Error!void {
        self.tracker.assertSame(self, "ove.TcpStream");
        try err.fromCode(c.ove_socket_listen(self.handle, backlog));
    }

    /// Accept an incoming connection into the caller-supplied `client`.
    /// `client` must arrive `undefined`; on success it is fully initialised
    /// and the caller becomes responsible for `client.deinit()`.
    pub fn accept(self: *TcpStream, client: *TcpStream, timeout_ms: u32) Error!void {
        self.tracker.assertSame(self, "ove.TcpStream");
        client.storage = pin.zeroStorage(c.ove_socket_storage_t);
        client.handle = null;
        client.tracker = .{};
        try err.fromCode(c.ove_socket_accept(
            self.handle,
            &client.handle,
            if (comptime pin.zero_heap) &client.storage else null,
            timeout_ms,
        ));
        client.tracker.record(client);
    }
};

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

/// UDP datagram socket.
///
/// ```zig
/// var sock: ove.UdpSocket = undefined;
/// try sock.init();
/// defer sock.deinit();
/// try sock.bind(ove.Address.any(1234));
/// ```
pub const UdpSocket = struct {
    storage: pin.Storage(c.ove_socket_storage_t),
    handle: c.ove_socket_t,
    tracker: pin.Tracker,

    pub fn init(self: *UdpSocket) Error!void {
        self.storage = pin.zeroStorage(c.ove_socket_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_socket_create(&self.handle, c.OVE_AF_INET, c.OVE_SOCK_DGRAM));
        } else {
            try err.fromCode(c.ove_socket_open(&self.handle, &self.storage, c.OVE_AF_INET, c.OVE_SOCK_DGRAM));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *UdpSocket) void {
        self.tracker.assertSame(self, "ove.UdpSocket");
        if (self.handle == null) return;
        if (comptime !pin.zero_heap)
            c.ove_socket_destroy(self.handle)
        else
            c.ove_socket_close(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn bind(self: *UdpSocket, addr: Address) Error!void {
        self.tracker.assertSame(self, "ove.UdpSocket");
        try err.fromCode(c.ove_socket_bind(self.handle, &addr.inner));
    }

    pub fn sendTo(self: *UdpSocket, data: []const u8, dest: Address) Error!usize {
        self.tracker.assertSame(self, "ove.UdpSocket");
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_sendto(self.handle, data.ptr, data.len, &sent, &dest.inner));
        return sent;
    }

    pub fn recvFrom(self: *UdpSocket, buf: []u8, timeout_ms: u32) Error!struct { len: usize, src: Address } {
        self.tracker.assertSame(self, "ove.UdpSocket");
        var received: usize = 0;
        var src: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_socket_recvfrom(self.handle, buf.ptr, buf.len, &received, &src, timeout_ms));
        return .{ .len = received, .src = .{ .inner = src } };
    }
};

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------

pub const dns = struct {
    pub fn resolve(hostname: [:0]const u8, timeout_ms: u32) Error!Address {
        var addr: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_dns_resolve(hostname.ptr, &addr, timeout_ms));
        return .{ .inner = addr };
    }
};
