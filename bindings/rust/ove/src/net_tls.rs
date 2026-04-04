// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! TLS/SSL session wrapper (mbedTLS).
//!
//! [`Session`] wraps the oveRTOS TLS handle with automatic cleanup.
//! After creating a session and completing the handshake over an existing
//! [`crate::net::TcpStream`], all I/O goes through [`Session::send`] and
//! [`Session::recv`].
//!
//! Works in both heap and zero-heap modes.

use crate::bindings;
use crate::error::{Error, Result};
use crate::net::TcpStream;

// ---------------------------------------------------------------------------
// TlsConfig
// ---------------------------------------------------------------------------

/// TLS session configuration.
///
/// `hostname` must be a null-terminated byte slice when provided (used for
/// SNI and certificate verification).
pub struct TlsConfig<'a> {
    /// PEM or DER CA certificate for server verification (`None` to skip).
    pub ca_cert: Option<&'a [u8]>,
    /// Expected server hostname for SNI/verify (null-terminated, or `None`).
    pub hostname: Option<&'a [u8]>,
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// TLS session with RAII cleanup.
///
/// Wraps `ove_tls_t` and frees resources on drop.
pub struct Session {
    handle: bindings::ove_tls_t,
}

impl Session {
    /// Create a new TLS session via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_tls_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_tls_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Session` and is not
    /// shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_tls_storage_t,
    ) -> Result<Self> {
        let mut handle: bindings::ove_tls_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_tls_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Perform the TLS handshake over an established TCP connection.
    ///
    /// After a successful handshake, use [`send`](Session::send) and
    /// [`recv`](Session::recv) for encrypted I/O.
    ///
    /// # Errors
    /// Returns an error if the handshake fails (certificate mismatch,
    /// protocol error, etc.).
    pub fn handshake(&self, sock: &TcpStream, cfg: &TlsConfig) -> Result<()> {
        let mut c: bindings::ove_tls_config_t = unsafe { core::mem::zeroed() };

        if let Some(cert) = cfg.ca_cert {
            c.ca_cert = cert.as_ptr();
            c.ca_cert_len = cert.len();
        }
        if let Some(host) = cfg.hostname {
            c.hostname = host.as_ptr() as *const _;
        }

        let rc = unsafe {
            bindings::ove_tls_handshake(self.handle, sock.handle(), &c)
        };
        Error::from_code(rc)
    }

    /// Send data over the encrypted session.
    ///
    /// Returns the number of bytes actually sent.
    ///
    /// # Errors
    /// Returns an error if the send fails.
    pub fn send(&self, data: &[u8]) -> Result<usize> {
        let mut sent: usize = 0;
        let rc = unsafe {
            bindings::ove_tls_send(
                self.handle,
                data.as_ptr() as *const _,
                data.len(),
                &mut sent,
            )
        };
        Error::from_code(rc)?;
        Ok(sent)
    }

    /// Receive data from the encrypted session.
    ///
    /// Returns the number of bytes received into `buf`.
    ///
    /// # Errors
    /// Returns an error if the receive fails or the peer closed the
    /// connection.
    pub fn recv(&self, buf: &mut [u8]) -> Result<usize> {
        let mut received: usize = 0;
        let rc = unsafe {
            bindings::ove_tls_recv(
                self.handle,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                &mut received,
            )
        };
        Error::from_code(rc)?;
        Ok(received)
    }

    /// Shut down the TLS session (sends `close_notify`).
    ///
    /// The underlying socket is NOT closed -- the caller must close it
    /// separately.
    pub fn close(&self) {
        unsafe { bindings::ove_tls_close(self.handle) }
    }
}

crate::ove_handle_impl!(Session, ove_tls_destroy, ove_tls_deinit, send_only);
