// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Networking primitives for oveRTOS.
//!
//! Provides typed wrappers for BSD-like sockets (TCP and UDP), network
//! interface management, DNS resolution, and socket addresses.
//! All resource types support both heap and zero-heap modes via the
//! standard comptime `@hasDecl` dispatch.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

// ---------------------------------------------------------------------------
// Address
// ---------------------------------------------------------------------------

/// Generic socket address (IPv4 or IPv6).  Value type — copyable, small.
pub const Address = struct {
    inner: c.ove_sockaddr_t,

    /// Create an IPv4 address from four octets and a port number.
    pub fn ipv4(a: u8, b: u8, addr_c: u8, d: u8, p: u16) Address {
        var addr: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        c.ove_sockaddr_ipv4(&addr, a, b, addr_c, d, p);
        return .{ .inner = addr };
    }

    /// Create a zeroed (any) address with the given port.
    pub fn any(p: u16) Address {
        return ipv4(0, 0, 0, 0, p);
    }

    /// Get the port number in host byte order.
    pub fn port(self: Address) u16 {
        return self.inner.port;
    }

    /// Return a copy with a different port.
    pub fn withPort(self: Address, p: u16) Address {
        var copy = self;
        copy.inner.port = p;
        return copy;
    }

    /// Get the first four address bytes (IPv4 octets).
    pub fn octets(self: Address) [4]u8 {
        return .{ self.inner.addr[0], self.inner.addr[1], self.inner.addr[2], self.inner.addr[3] };
    }
};

// ---------------------------------------------------------------------------
// NetIfConfig
// ---------------------------------------------------------------------------

/// Network interface configuration with immutable builder pattern.
pub const NetIfConfig = struct {
    inner: c.ove_netif_config_t,

    /// Create a zeroed configuration (no DHCP, no static IP, no DNS).
    pub fn init() NetIfConfig {
        return .{ .inner = std.mem.zeroes(c.ove_netif_config_t) };
    }

    /// Enable DHCP.
    pub fn dhcp(self: NetIfConfig) NetIfConfig {
        var copy = self;
        copy.inner.use_dhcp = 1;
        return copy;
    }

    /// Set static IP configuration (disables DHCP).
    pub fn staticIp(self: NetIfConfig, ip: Address, mask: Address, gw: Address) NetIfConfig {
        var copy = self;
        copy.inner.use_dhcp = 0;
        copy.inner.static_ip = ip.inner;
        copy.inner.netmask = mask.inner;
        copy.inner.gateway = gw.inner;
        return copy;
    }

    /// Set the DNS server address.
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
/// In zero-heap mode, storage is allocated in a comptime-unique static
/// variable — only one instance per call site.
pub const NetIf = struct {
    handle: c.ove_netif_t,

    /// Create a network interface.
    pub fn create() Error!NetIf {
        var h: c.ove_netif_t = null;
        if (comptime @hasDecl(c, "ove_netif_create")) {
            try err.fromCode(c.ove_netif_create(&h));
        } else {
            const S = struct {
                var storage: c.ove_netif_storage_t = std.mem.zeroes(c.ove_netif_storage_t);
            };
            try err.fromCode(c.ove_netif_init(&h, &S.storage));
        }
        return .{ .handle = h };
    }

    /// Destroy the network interface.
    pub fn destroy(self: *NetIf) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_netif_destroy"))
            c.ove_netif_destroy(self.handle)
        else
            c.ove_netif_deinit(self.handle);
        self.handle = null;
    }

    /// Bring the interface up with the given configuration.
    pub fn up(self: NetIf, cfg: NetIfConfig) Error!void {
        try err.fromCode(c.ove_netif_up(self.handle, &cfg.inner));
    }

    /// Tear down the interface.
    pub fn down(self: NetIf) void {
        c.ove_netif_down(self.handle);
    }

    /// IP/gateway/netmask address triple returned by `getAddr()`.
    pub const AddrInfo = struct { ip: Address, gateway: Address, netmask: Address };

    /// Query the current IP, gateway, and netmask of this interface.
    pub fn getAddr(self: NetIf) Error!AddrInfo {
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
/// In zero-heap mode, storage is allocated in a comptime-unique static
/// variable — only one instance per call site.
pub const TcpStream = struct {
    handle: c.ove_socket_t,

    /// Open a TCP socket.
    pub fn create() Error!TcpStream {
        var h: c.ove_socket_t = null;
        if (comptime @hasDecl(c, "ove_socket_create")) {
            try err.fromCode(c.ove_socket_create(&h, c.OVE_AF_INET, c.OVE_SOCK_STREAM));
        } else {
            const S = struct {
                var storage: c.ove_socket_storage_t = std.mem.zeroes(c.ove_socket_storage_t);
            };
            try err.fromCode(c.ove_socket_open(&h, &S.storage, c.OVE_AF_INET, c.OVE_SOCK_STREAM));
        }
        return .{ .handle = h };
    }

    /// Close the TCP socket.
    pub fn destroy(self: *TcpStream) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_socket_destroy"))
            c.ove_socket_destroy(self.handle)
        else
            c.ove_socket_close(self.handle);
        self.handle = null;
    }

    /// Connect to a remote address.
    pub fn connect(self: TcpStream, addr: Address, timeout_ms: u32) Error!void {
        try err.fromCode(c.ove_socket_connect(self.handle, &addr.inner, timeout_ms));
    }

    /// Send data. Returns number of bytes sent.
    pub fn send(self: TcpStream, data: []const u8) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_send(self.handle, data.ptr, data.len, &sent));
        return sent;
    }

    /// Receive data. Returns number of bytes received.
    pub fn recv(self: TcpStream, buf: []u8, timeout_ms: u32) Error!usize {
        var received: usize = 0;
        try err.fromCode(c.ove_socket_recv(self.handle, buf.ptr, buf.len, &received, timeout_ms));
        return received;
    }

    /// Bind to a local address (for server use before listen).
    pub fn bind(self: TcpStream, addr: Address) Error!void {
        try err.fromCode(c.ove_socket_bind(self.handle, &addr.inner));
    }

    /// Mark socket as listening.
    pub fn listen(self: TcpStream, backlog: i32) Error!void {
        try err.fromCode(c.ove_socket_listen(self.handle, backlog));
    }

    /// Accept an incoming connection. Returns a new TcpStream for the client.
    ///
    /// In zero-heap mode, only one accepted socket per call site is supported.
    pub fn accept(self: TcpStream, timeout_ms: u32) Error!TcpStream {
        var client_h: c.ove_socket_t = null;
        const CS = struct {
            var client_storage: c.ove_socket_storage_t = std.mem.zeroes(c.ove_socket_storage_t);
        };
        try err.fromCode(c.ove_socket_accept(self.handle, &client_h, &CS.client_storage, timeout_ms));
        return .{ .handle = client_h };
    }
};

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

/// UDP datagram socket.
///
/// In zero-heap mode, storage is allocated in a comptime-unique static
/// variable — only one instance per call site.
pub const UdpSocket = struct {
    handle: c.ove_socket_t,

    /// Open a UDP socket.
    pub fn create() Error!UdpSocket {
        var h: c.ove_socket_t = null;
        if (comptime @hasDecl(c, "ove_socket_create")) {
            try err.fromCode(c.ove_socket_create(&h, c.OVE_AF_INET, c.OVE_SOCK_DGRAM));
        } else {
            const S = struct {
                var storage: c.ove_socket_storage_t = std.mem.zeroes(c.ove_socket_storage_t);
            };
            try err.fromCode(c.ove_socket_open(&h, &S.storage, c.OVE_AF_INET, c.OVE_SOCK_DGRAM));
        }
        return .{ .handle = h };
    }

    /// Close the UDP socket.
    pub fn destroy(self: *UdpSocket) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_socket_destroy"))
            c.ove_socket_destroy(self.handle)
        else
            c.ove_socket_close(self.handle);
        self.handle = null;
    }

    /// Bind to a local address.
    pub fn bind(self: UdpSocket, addr: Address) Error!void {
        try err.fromCode(c.ove_socket_bind(self.handle, &addr.inner));
    }

    /// Send a datagram to a destination. Returns bytes sent.
    pub fn sendTo(self: UdpSocket, data: []const u8, dest: Address) Error!usize {
        var sent: usize = 0;
        try err.fromCode(c.ove_socket_sendto(self.handle, data.ptr, data.len, &sent, &dest.inner));
        return sent;
    }

    /// Receive a datagram. Returns bytes received and source address.
    pub fn recvFrom(self: UdpSocket, buf: []u8, timeout_ms: u32) Error!struct { len: usize, src: Address } {
        var received: usize = 0;
        var src: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_socket_recvfrom(self.handle, buf.ptr, buf.len, &received, &src, timeout_ms));
        return .{ .len = received, .src = .{ .inner = src } };
    }
};

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------

/// DNS resolution.
pub const dns = struct {
    /// Resolve a hostname to an address.
    ///
    /// `hostname` must be a sentinel-terminated string.
    pub fn resolve(hostname: [:0]const u8, timeout_ms: u32) Error!Address {
        var addr: c.ove_sockaddr_t = std.mem.zeroes(c.ove_sockaddr_t);
        try err.fromCode(c.ove_dns_resolve(hostname.ptr, &addr, timeout_ms));
        return .{ .inner = addr };
    }
};
