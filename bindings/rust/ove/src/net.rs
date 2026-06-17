// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Blocking BSD-style networking primitives for oveRTOS.
//!
//! Provides safe wrappers for sockets (TCP and UDP), network interface
//! management, DNS resolution, and socket addresses. All types use
//! inline storage and implement `Send + Sync`.
//!
//! Each backend uses its native TCP/IP stack:
//!
//! | Backend  | Stack         | Notes |
//! |----------|---------------|-------|
//! | FreeRTOS | lwIP (vendored) | Bare-metal target; sockets in oveRTOS heap |
//! | Zephyr   | Zephyr `net`  | Uses Zephyr's POSIX `sockets` shim |
//! | NuttX    | NuttX net     | Native socket layer |
//! | POSIX    | host kernel   | Native `socket(2)` via libc |
//!
//! ## When to pick `ove::net` (blocking) vs [`crate::async_net`]
//!
//! Both stacks coexist as separate Cargo features ([`crate::net`] is
//! always available; [`crate::async_net`] requires `async-net` +
//! `CONFIG_OVE_ASYNC_NET=y`), but at build time they are **mutually
//! exclusive at the transport layer**: lwIP (or Zephyr/NuttX net) and
//! embassy-net both want to own the MAC + ARP cache + descriptor ring.
//! Pick **one** stack per build.
//!
//! Choose `ove::net` (this module) when:
//!
//! - You're writing **synchronous code** in a dedicated networking
//!   thread (the common bare-metal pattern). One thread, one socket,
//!   one blocking `recv` per loop iteration.
//! - You need **TLS + HTTP + MQTT + SNTP + HTTPD** all in one app and
//!   want production-tested implementations — the [`crate::net_tls`] /
//!   [`crate::net_http`] / [`crate::net_mqtt`] / [`crate::net_sntp`] /
//!   [`crate::net_httpd`] sister modules wrap mbedTLS + battle-tested
//!   C clients that are part of oveRTOS itself.
//! - You're on a backend where the **native socket layer is mature**
//!   (Zephyr/NuttX/POSIX). The HAL is doing the heavy lifting; the
//!   Rust wrapper is just type-safe glue.
//! - You're **interoperating with existing C code** that already
//!   opens sockets, calls DNS, etc.
//!
//! Choose [`crate::async_net`] (Embassy + embassy-net) when:
//!
//! - You're writing **multiple concurrent tasks** that all do network
//!   I/O — TCP listener + MQTT client + sensor uplink — and don't
//!   want one thread per task.
//! - You want **embedded-hal-async / embedded-io-async** trait impls
//!   so async sensor drivers from crates.io work directly on top of
//!   your sockets.
//! - You're on **bare-metal FreeRTOS** (the lwIP path is fine, but
//!   embassy-net is smaller and gives you the Rust-native API).
//! - You're using async elsewhere ([`crate::timer::Timer`] →
//!   `Timer::after_millis().await`, `embedded_io_async::Write`) and
//!   want a single async story end-to-end.
//!
//! ## Protocol layers
//!
//! oveRTOS ships C clients for the common app-layer protocols, with
//! Rust wrappers that hand back RAII-cleaning handles:
//!
//! - [`crate::net_tls`] — mbedTLS sessions on top of [`TcpStream`]
//! - [`crate::net_http`] — HTTP/1.1 client with `get`/`post`/`put`,
//!   automatic redirect handling, gzip decode
//! - [`crate::net_mqtt`] — MQTT 3.1.1 client (QoS 0/1, keep-alive,
//!   optional TLS)
//! - [`crate::net_sntp`] — SNTP time sync (single-shot or periodic)
//! - [`crate::net_httpd`] — embedded HTTP server with REST routes +
//!   WebSocket upgrade
//!
//! For the async equivalents, see [`crate::async_net`]'s module docs —
//! it documents which crates.io community crates pair with embassy-net
//! ([`rust-mqtt`], [`reqwless`], [`embedded-tls`], `sntpc`, `picoserve`).

use core::fmt;

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY (module-wide contract for the `unsafe { bindings::ove_*(...) }` FFI
// calls below): any handle passed to the C API is non-null and refers to a
// live RTOS object — wrapper constructors establish validity via
// `Error::from_code`, and `Drop` (or an explicit `deinit`) is the only place
// a handle is released. Pointer and slice arguments reference caller-owned
// memory valid for the duration of the call; the C side copies whatever it
// retains and does not alias them past return (verified against the
// signatures in `include/ove/*.h`). Blocks that deviate — `transmute`, raw
// pointer casts from user data, slice reconstruction via `from_raw_parts`,
// or storing a callback across the FFI boundary — carry their own
// `// SAFETY:` comment.

// ---------------------------------------------------------------------------
// Address
// ---------------------------------------------------------------------------

/// Generic socket address (IPv4 or IPv6).
///
/// Wraps [`ove_sockaddr_t`](bindings::ove_sockaddr_t) transparently so it can
/// be passed directly to the C API without conversion.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct Address(bindings::ove_sockaddr_t);

impl Address {
    /// Create an IPv4 address from four octets and a port number.
    pub fn ipv4(a: u8, b: u8, c: u8, d: u8, port: u16) -> Self {
        let mut inner: bindings::ove_sockaddr_t = unsafe { core::mem::zeroed() };
        unsafe { bindings::ove_sockaddr_ipv4(&mut inner, a, b, c, d, port) };
        Self(inner)
    }

    /// Get the port number in host byte order.
    pub fn port(&self) -> u16 {
        self.0.port
    }

    /// Set the port number in host byte order.
    pub fn set_port(&mut self, port: u16) {
        self.0.port = port;
    }

    /// Get the first four bytes of the address (the IPv4 octets).
    pub fn octets(&self) -> [u8; 4] {
        [
            self.0.addr[0],
            self.0.addr[1],
            self.0.addr[2],
            self.0.addr[3],
        ]
    }

    /// Get a const pointer to the inner sockaddr for passing to C APIs.
    pub(crate) fn as_ptr(&self) -> *const bindings::ove_sockaddr_t {
        &self.0
    }

    /// Get a mutable pointer to the inner sockaddr for receiving from C APIs.
    pub(crate) fn as_mut_ptr(&mut self) -> *mut bindings::ove_sockaddr_t {
        &mut self.0
    }

    /// Borrow the inner C struct.
    fn as_inner(&self) -> &bindings::ove_sockaddr_t {
        &self.0
    }
}

impl Default for Address {
    fn default() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }
}

impl fmt::Debug for Address {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let o = self.octets();
        f.debug_struct("Address")
            .field("ip", &format_args!("{}.{}.{}.{}", o[0], o[1], o[2], o[3]))
            .field("port", &self.0.port)
            .finish()
    }
}

// ---------------------------------------------------------------------------
// NetIfConfig
// ---------------------------------------------------------------------------

/// Builder for network interface configuration.
///
/// Wraps [`ove_netif_config_t`](bindings::ove_netif_config_t) and provides a
/// fluent API for configuring DHCP, static IP, and DNS.
pub struct NetIfConfig(bindings::ove_netif_config_t);

impl NetIfConfig {
    /// Create a zeroed configuration (no DHCP, no static IP, no DNS).
    pub fn new() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }

    /// Configure a static IP, subnet mask, and gateway.
    ///
    /// Disables DHCP.
    pub fn static_ip(mut self, ip: Address, mask: Address, gw: Address) -> Self {
        self.0.use_dhcp = 0;
        self.0.static_ip = *ip.as_inner();
        self.0.netmask = *mask.as_inner();
        self.0.gateway = *gw.as_inner();
        self
    }

    /// Enable DHCP for automatic address assignment.
    pub fn dhcp(mut self) -> Self {
        self.0.use_dhcp = 1;
        self
    }

    /// Set the DNS server address.
    pub fn dns(mut self, dns: Address) -> Self {
        self.0.dns = *dns.as_inner();
        self
    }
}

impl Default for NetIfConfig {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// NetIf
// ---------------------------------------------------------------------------

/// RAII wrapper around a network interface.
///
/// Uses inline storage so no heap/zero-heap split is needed.
/// The handle is derived from `&self.storage` at each use to avoid
/// dangling self-referential pointers after Rust moves the struct.
pub struct NetIf {
    storage: bindings::ove_netif_storage_t,
    init: bool,
}

impl NetIf {
    /// Derive the C handle from inline storage.
    fn handle(&self) -> bindings::ove_netif_t {
        &self.storage as *const _ as bindings::ove_netif_t
    }

    /// Initialise a network interface using inline storage.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS rejects the initialisation.
    pub fn new() -> Result<Self> {
        let mut netif = Self {
            storage: unsafe { core::mem::zeroed() },
            init: false,
        };
        let mut handle: bindings::ove_netif_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_netif_init(&mut handle, &mut netif.storage) };
        Error::from_code(rc)?;
        netif.init = true;
        Ok(netif)
    }

    /// Bring the interface up with the given configuration.
    ///
    /// # Errors
    /// Returns an error if the RTOS cannot bring the interface up (e.g.
    /// invalid configuration or hardware failure).
    pub fn up(&self, cfg: &NetIfConfig) -> Result<()> {
        let rc = unsafe { bindings::ove_netif_up(self.handle(), &cfg.0) };
        Error::from_code(rc)
    }

    /// Tear down the network interface.
    pub fn down(&self) {
        unsafe { bindings::ove_netif_down(self.handle()) }
    }

    /// Query current addresses of the network interface.
    ///
    /// Returns `(ip, gateway, netmask)` on success.
    ///
    /// # Errors
    /// Returns an error if the RTOS cannot retrieve the addresses.
    pub fn get_addr(&self) -> Result<(Address, Address, Address)> {
        let mut ip = core::mem::MaybeUninit::<bindings::ove_sockaddr_t>::zeroed();
        let mut gw = core::mem::MaybeUninit::<bindings::ove_sockaddr_t>::zeroed();
        let mut nm = core::mem::MaybeUninit::<bindings::ove_sockaddr_t>::zeroed();
        let rc = unsafe {
            bindings::ove_netif_get_addr(
                self.handle(),
                ip.as_mut_ptr(),
                gw.as_mut_ptr(),
                nm.as_mut_ptr(),
            )
        };
        Error::from_code(rc)?;
        unsafe {
            Ok((
                Address(ip.assume_init()),
                Address(gw.assume_init()),
                Address(nm.assume_init()),
            ))
        }
    }
}

impl fmt::Debug for NetIf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("NetIf")
            .field("handle", &format_args!("{:p}", self.handle()))
            .finish()
    }
}

impl Drop for NetIf {
    fn drop(&mut self) {
        if !self.init {
            return;
        }
        unsafe { bindings::ove_netif_deinit(self.handle()) }
    }
}

// SAFETY: Wraps a ove handle backed by inline storage. The RTOS net calls are
// thread-safe. Create/destroy are single-threaded (lifecycle guarantee).
unsafe impl Send for NetIf {}
unsafe impl Sync for NetIf {}

// ---------------------------------------------------------------------------
// TcpStream
// ---------------------------------------------------------------------------

/// RAII wrapper around a TCP (stream) socket.
///
/// Uses inline storage for the socket backend, so no heap/zero-heap split is
/// needed.  The handle is derived from `&self.storage` at each call to
/// avoid dangling self-referential pointers after Rust moves the struct.
pub struct TcpStream {
    storage: bindings::ove_socket_storage_t,
    open: bool,
}

impl TcpStream {
    /// Derive the C handle from inline storage.
    pub(crate) fn handle(&self) -> bindings::ove_socket_t {
        &self.storage as *const _ as bindings::ove_socket_t
    }

    /// Open a new TCP socket.
    ///
    /// # Errors
    /// Returns an error if the RTOS cannot open the socket.
    pub fn new() -> Result<Self> {
        let mut sock = Self {
            storage: unsafe { core::mem::zeroed() },
            open: false,
        };
        let mut handle: bindings::ove_socket_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_socket_open(
                &mut handle,
                &mut sock.storage,
                bindings::OVE_AF_INET,
                bindings::OVE_SOCK_STREAM,
            )
        };
        Error::from_code(rc)?;
        sock.open = true;
        Ok(sock)
    }

    /// Connect to a remote address with a timeout.
    ///
    /// # Errors
    /// Returns an error if the connection fails or times out.
    pub fn connect(&self, addr: &Address, timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_socket_connect(
                self.handle(),
                addr.as_ptr(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Send data on the connected socket.
    ///
    /// Returns the number of bytes actually sent.
    ///
    /// # Errors
    /// Returns an error if the send fails.
    pub fn send(&self, data: &[u8]) -> Result<usize> {
        let mut sent: usize = 0;
        let rc = unsafe {
            bindings::ove_socket_send(
                self.handle(),
                data.as_ptr() as *const _,
                data.len(),
                &mut sent,
            )
        };
        Error::from_code(rc)?;
        Ok(sent)
    }

    /// Send the entire buffer, looping over [`send`](Self::send) until
    /// every byte has been written.
    ///
    /// A single [`send`](Self::send) may write only part of the buffer;
    /// this drives the partial-write loop so callers don't have to track
    /// the unsent tail themselves.
    ///
    /// # Errors
    /// Returns the first error from [`send`](Self::send).
    pub fn send_all(&self, data: &[u8]) -> Result<()> {
        let mut total = 0;
        while total < data.len() {
            total += self.send(&data[total..])?;
        }
        Ok(())
    }

    /// Receive data from the connected socket.
    ///
    /// Returns the number of bytes received.
    ///
    /// # Errors
    /// Returns an error if the receive fails or times out.
    pub fn recv(&self, buf: &mut [u8], timeout: core::time::Duration) -> Result<usize> {
        let mut received: usize = 0;
        let rc = unsafe {
            bindings::ove_socket_recv(
                self.handle(),
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                &mut received,
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)?;
        Ok(received)
    }
}

impl fmt::Debug for TcpStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TcpStream")
            .field("handle", &format_args!("{:p}", self.handle()))
            .field("open", &self.open)
            .finish_non_exhaustive()
    }
}

impl Drop for TcpStream {
    fn drop(&mut self) {
        if self.open {
            unsafe { bindings::ove_socket_close(self.handle()) }
        }
    }
}

// SAFETY: Wraps a ove handle backed by inline storage. The RTOS socket calls
// are thread-safe. Open/close are single-threaded (lifecycle guarantee).
unsafe impl Send for TcpStream {}
unsafe impl Sync for TcpStream {}

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

/// RAII wrapper around a UDP (datagram) socket.
///
/// Uses inline storage for the socket backend, so no heap/zero-heap split is
/// needed.  The handle is derived from `&self.storage` at each call to
/// avoid dangling self-referential pointers after Rust moves the struct.
pub struct UdpSocket {
    storage: bindings::ove_socket_storage_t,
    open: bool,
}

impl UdpSocket {
    /// Derive the C handle from inline storage.
    fn handle(&self) -> bindings::ove_socket_t {
        &self.storage as *const _ as bindings::ove_socket_t
    }

    /// Open a new UDP socket.
    ///
    /// # Errors
    /// Returns an error if the RTOS cannot open the socket.
    pub fn new() -> Result<Self> {
        let mut sock = Self {
            storage: unsafe { core::mem::zeroed() },
            open: false,
        };
        let mut handle: bindings::ove_socket_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_socket_open(
                &mut handle,
                &mut sock.storage,
                bindings::OVE_AF_INET,
                bindings::OVE_SOCK_DGRAM,
            )
        };
        Error::from_code(rc)?;
        sock.open = true;
        Ok(sock)
    }

    /// Bind the socket to a local address.
    ///
    /// # Errors
    /// Returns an error if the bind fails (e.g. address already in use).
    pub fn bind(&self, addr: &Address) -> Result<()> {
        let rc = unsafe { bindings::ove_socket_bind(self.handle(), addr.as_ptr()) };
        Error::from_code(rc)
    }

    /// Send a datagram to a specific destination.
    ///
    /// Returns the number of bytes actually sent.
    ///
    /// # Errors
    /// Returns an error if the send fails.
    pub fn send_to(&self, data: &[u8], dest: &Address) -> Result<usize> {
        let mut sent: usize = 0;
        let rc = unsafe {
            bindings::ove_socket_sendto(
                self.handle(),
                data.as_ptr() as *const _,
                data.len(),
                &mut sent,
                dest.as_ptr(),
            )
        };
        Error::from_code(rc)?;
        Ok(sent)
    }

    /// Receive a datagram and the sender's address.
    ///
    /// Returns the number of bytes received and the source address.
    ///
    /// # Errors
    /// Returns an error if the receive fails or times out.
    pub fn recv_from(
        &self,
        buf: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<(usize, Address)> {
        let mut received: usize = 0;
        let mut src = Address::default();
        let rc = unsafe {
            bindings::ove_socket_recvfrom(
                self.handle(),
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                &mut received,
                src.as_mut_ptr(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)?;
        Ok((received, src))
    }
}

impl fmt::Debug for UdpSocket {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("UdpSocket")
            .field("handle", &format_args!("{:p}", self.handle()))
            .field("open", &self.open)
            .finish_non_exhaustive()
    }
}

impl Drop for UdpSocket {
    fn drop(&mut self) {
        if self.open {
            unsafe { bindings::ove_socket_close(self.handle()) }
        }
    }
}

// SAFETY: Wraps a ove handle backed by inline storage. The RTOS socket calls
// are thread-safe. Open/close are single-threaded (lifecycle guarantee).
unsafe impl Send for UdpSocket {}
unsafe impl Sync for UdpSocket {}

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------

/// Resolve a hostname to an address.
///
/// The `hostname` is passed as a `&CStr` (e.g. a `c"example.com"` literal).
///
/// # Errors
/// Returns an error if name resolution fails or times out.
pub fn dns_resolve(hostname: &core::ffi::CStr, timeout: core::time::Duration) -> Result<Address> {
    let mut addr = Address::default();
    let rc = unsafe {
        bindings::ove_dns_resolve(
            hostname.as_ptr() as *const _,
            addr.as_mut_ptr(),
            crate::time::dur_to_ns(timeout),
        )
    };
    Error::from_code(rc)?;
    Ok(addr)
}
