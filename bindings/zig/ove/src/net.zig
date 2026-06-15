// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Networking primitives for oveRTOS.
//!
//! Provides typed wrappers for BSD-like sockets (TCP and UDP), network
//! interface management, DNS resolution, and socket addresses.  Resource
//! types use a per-mode shape: heap mode is value-returning and movable;
//! zero-heap mode embeds storage and uses two-phase init.  Cleanup is
//! single-owner in both modes: `deinit` takes `*Self`, clears the handle,
//! and is idempotent — a redundant `defer x.deinit()` after an explicit
//! `deinit()` is a safe no-op.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

// ---------------------------------------------------------------------------
// Address  (value type, no kernel resource)
// ---------------------------------------------------------------------------

/// Socket address.  Value type — copyable, small.  The underlying
/// `ove_sockaddr_t` carries a 16-byte address buffer (IPv4 + IPv6
/// capable on the C side), but the Zig-side constructors and
/// accessors here are IPv4-only — `ipv4(...)` builds an address,
/// `octets()` returns the first 4 bytes.
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
/// Heap mode (value-returning):
///
/// ```zig
/// var nif = try ove.NetIf.create();
/// defer nif.deinit();
/// try nif.up(ove.NetIfConfig.init().dhcp());
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var nif: ove.NetIf = undefined;
/// try nif.init();
/// defer nif.deinit();
/// ```
pub const NetIf = if (pin.zero_heap) ZeroHeapNetIf else HeapNetIf;

const HeapNetIf = struct {
    handle: c.ove_netif_t,

    pub fn create() Error!NetIf {
        var h: c.ove_netif_t = null;
        try err.fromCode(c.ove_netif_create(&h));
        return .{ .handle = h };
    }

    /// Idempotent — clears `handle` after destroy so `defer deinit()`
    /// after an explicit `deinit()` is safe (the second call sees a
    /// null handle and short-circuits, avoiding a double free that
    /// would corrupt the kmm freelist).
    pub fn deinit(self: *NetIf) void {
        if (self.handle == null) return;
        c.ove_netif_destroy(self.handle);
        self.handle = null;
    }

    pub fn up(self: *NetIf, cfg: NetIfConfig) Error!void {
        try err.fromCode(c.ove_netif_up(self.handle, &cfg.inner));
    }

    pub fn down(self: *NetIf) void {
        c.ove_netif_down(self.handle);
    }

    pub const AddrInfo = struct { ip: Address, gateway: Address, netmask: Address };

    pub fn getAddr(self: *NetIf) Error!AddrInfo {
        var ip = std.mem.zeroes(c.ove_sockaddr_t);
        var gw = std.mem.zeroes(c.ove_sockaddr_t);
        var nm = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_netif_get_addr(self.handle, &ip, &gw, &nm));
        return .{ .ip = .{ .inner = ip }, .gateway = .{ .inner = gw }, .netmask = .{ .inner = nm } };
    }
};

const ZeroHeapNetIf = struct {
    storage: c.ove_netif_storage_t,
    handle: c.ove_netif_t,
    tracker: pin.Tracker,

    pub fn init(self: *NetIf) Error!void {
        self.storage = std.mem.zeroes(c.ove_netif_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_netif_init(&self.handle, &self.storage));
        self.tracker.record(self);
    }

    pub fn deinit(self: *NetIf) void {
        self.tracker.assertSame(self, "ove.NetIf");
        if (self.handle == null) return;
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
pub const TcpStream = if (pin.zero_heap) ZeroHeapTcpStream else HeapTcpStream;

const HeapTcpStream = struct {
    handle: c.ove_socket_t,

    pub fn create() Error!TcpStream {
        var h: c.ove_socket_t = null;
        try err.fromCode(c.ove_socket_create(&h, c.OVE_AF_INET, c.OVE_SOCK_STREAM));
        return .{ .handle = h };
    }

    /// Idempotent — see HeapNetIf.deinit.
    pub fn deinit(self: *TcpStream) void {
        if (self.handle == null) return;
        c.ove_socket_destroy(self.handle);
        self.handle = null;
    }

    pub fn connect(self: *TcpStream, addr: Address, timeout_ns: u64) Error!void {
        try err.fromCode(c.ove_socket_connect(self.handle, &addr.inner, timeout_ns));
    }

    pub fn send(self: *TcpStream, data: []const u8) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_send(self.handle, data.ptr, data.len, &sent));
        return sent;
    }

    pub fn recv(self: *TcpStream, buf: []u8, timeout_ns: u64) Error!usize {
        var received: usize = 0;
        try err.fromCode(c.ove_socket_recv(self.handle, buf.ptr, buf.len, &received, timeout_ns));
        return received;
    }

    pub fn bind(self: *TcpStream, addr: Address) Error!void {
        try err.fromCode(c.ove_socket_bind(self.handle, &addr.inner));
    }

    pub fn listen(self: *TcpStream, backlog: i32) Error!void {
        try err.fromCode(c.ove_socket_listen(self.handle, backlog));
    }

    pub fn accept(self: *TcpStream, timeout_ns: u64) Error!TcpStream {
        var h: c.ove_socket_t = null;
        try err.fromCode(c.ove_socket_accept(self.handle, &h, null, timeout_ns));
        return .{ .handle = h };
    }
};

const ZeroHeapTcpStream = struct {
    storage: c.ove_socket_storage_t,
    handle: c.ove_socket_t,
    tracker: pin.Tracker,

    pub fn init(self: *TcpStream) Error!void {
        self.storage = std.mem.zeroes(c.ove_socket_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_socket_open(&self.handle, &self.storage, c.OVE_AF_INET, c.OVE_SOCK_STREAM));
        self.tracker.record(self);
    }

    pub fn deinit(self: *TcpStream) void {
        self.tracker.assertSame(self, "ove.TcpStream");
        if (self.handle == null) return;
        c.ove_socket_close(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn connect(self: *TcpStream, addr: Address, timeout_ns: u64) Error!void {
        self.tracker.assertSame(self, "ove.TcpStream");
        try err.fromCode(c.ove_socket_connect(self.handle, &addr.inner, timeout_ns));
    }

    pub fn send(self: *TcpStream, data: []const u8) Error!usize {
        self.tracker.assertSame(self, "ove.TcpStream");
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_send(self.handle, data.ptr, data.len, &sent));
        return sent;
    }

    pub fn recv(self: *TcpStream, buf: []u8, timeout_ns: u64) Error!usize {
        self.tracker.assertSame(self, "ove.TcpStream");
        var received: usize = 0;
        try err.fromCode(c.ove_socket_recv(self.handle, buf.ptr, buf.len, &received, timeout_ns));
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
    pub fn accept(self: *TcpStream, client: *TcpStream, timeout_ns: u64) Error!void {
        self.tracker.assertSame(self, "ove.TcpStream");
        client.storage = std.mem.zeroes(c.ove_socket_storage_t);
        client.handle = null;
        client.tracker = .{};
        try err.fromCode(c.ove_socket_accept(self.handle, &client.handle, &client.storage, timeout_ns));
        client.tracker.record(client);
    }
};

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

/// UDP datagram socket.
pub const UdpSocket = if (pin.zero_heap) ZeroHeapUdpSocket else HeapUdpSocket;

const HeapUdpSocket = struct {
    handle: c.ove_socket_t,

    pub fn create() Error!UdpSocket {
        var h: c.ove_socket_t = null;
        try err.fromCode(c.ove_socket_create(&h, c.OVE_AF_INET, c.OVE_SOCK_DGRAM));
        return .{ .handle = h };
    }

    /// Idempotent — see HeapNetIf.deinit.
    pub fn deinit(self: *UdpSocket) void {
        if (self.handle == null) return;
        c.ove_socket_destroy(self.handle);
        self.handle = null;
    }

    pub fn bind(self: *UdpSocket, addr: Address) Error!void {
        try err.fromCode(c.ove_socket_bind(self.handle, &addr.inner));
    }

    pub fn sendTo(self: *UdpSocket, data: []const u8, dest: Address) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_sendto(self.handle, data.ptr, data.len, &sent, &dest.inner));
        return sent;
    }

    pub fn recvFrom(self: *UdpSocket, buf: []u8, timeout_ns: u64) Error!struct { len: usize, src: Address } {
        var received: usize = 0;
        var src: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_socket_recvfrom(self.handle, buf.ptr, buf.len, &received, &src, timeout_ns));
        return .{ .len = received, .src = .{ .inner = src } };
    }
};

const ZeroHeapUdpSocket = struct {
    storage: c.ove_socket_storage_t,
    handle: c.ove_socket_t,
    tracker: pin.Tracker,

    pub fn init(self: *UdpSocket) Error!void {
        self.storage = std.mem.zeroes(c.ove_socket_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_socket_open(&self.handle, &self.storage, c.OVE_AF_INET, c.OVE_SOCK_DGRAM));
        self.tracker.record(self);
    }

    pub fn deinit(self: *UdpSocket) void {
        self.tracker.assertSame(self, "ove.UdpSocket");
        if (self.handle == null) return;
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

    pub fn recvFrom(self: *UdpSocket, buf: []u8, timeout_ns: u64) Error!struct { len: usize, src: Address } {
        self.tracker.assertSame(self, "ove.UdpSocket");
        var received: usize = 0;
        var src: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_socket_recvfrom(self.handle, buf.ptr, buf.len, &received, &src, timeout_ns));
        return .{ .len = received, .src = .{ .inner = src } };
    }
};

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------

pub const dns = struct {
    pub fn resolve(hostname: [:0]const u8, timeout_ns: u64) Error!Address {
        var addr: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_dns_resolve(hostname.ptr, &addr, timeout_ns));
        return .{ .inner = addr };
    }
};
