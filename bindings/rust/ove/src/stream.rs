// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Byte-stream buffer for oveRTOS.
//!
//! [`Stream<N>`] is a lock-free ring buffer for passing arbitrary byte sequences
//! between threads or between ISR and thread contexts. Unlike [`crate::Queue`], which
//! transfers discrete items, a `Stream` treats data as a continuous byte flow with
//! a configurable receive-trigger threshold.

use crate::bindings;
use crate::error::{Error, Result};

/// Byte-oriented stream buffer with compile-time buffer size.
///
/// Wraps `ove_stream_t` for passing variable-length byte data between
/// threads or between ISR and thread contexts. `N` is the buffer size in bytes.
pub struct Stream<const N: usize> {
    handle: bindings::ove_stream_t,
}

impl<const N: usize> Stream<N> {
    /// Create a stream via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new(trigger: usize) -> Result<Self> {
        let mut handle: bindings::ove_stream_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_stream_create(&mut handle, N, trigger) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage and buffer.
    ///
    /// # Safety
    /// - `storage` must outlive the `Stream` and not be shared.
    /// - `buffer` must point to at least `N` bytes and outlive the `Stream`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_stream_storage_t,
        buffer: *mut core::ffi::c_void,
        trigger: usize,
    ) -> Result<Self> {
        let mut handle: bindings::ove_stream_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_stream_init(&mut handle, storage, buffer, N, trigger) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Send bytes into the stream, blocking up to `timeout_ms` if the buffer is full.
    ///
    /// Returns the number of bytes actually sent, which may be less than `data.len()`
    /// if the stream fills before the timeout.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if no bytes could be sent within `timeout_ms`.
    #[inline]
    pub fn send(&self, data: &[u8], timeout_ms: u32) -> Result<usize> {
        let mut bytes_sent: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_send(
                self.handle,
                data.as_ptr() as *const _,
                data.len(),
                timeout_ms,
                &mut bytes_sent,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_sent)
    }

    /// Receive bytes from the stream into `buf`, blocking up to `timeout_ms`.
    ///
    /// Returns the number of bytes actually received. Blocks until at least the
    /// trigger byte count (set at creation time) is available, or `timeout_ms` expires.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if no bytes could be received within `timeout_ms`.
    #[inline]
    pub fn receive(&self, buf: &mut [u8], timeout_ms: u32) -> Result<usize> {
        let mut bytes_received: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_receive(
                self.handle,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                timeout_ms,
                &mut bytes_received,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_received)
    }

    /// Send bytes from an ISR context (non-blocking, returns immediately).
    ///
    /// Returns the number of bytes sent; may be less than `data.len()` if the buffer fills.
    ///
    /// # Errors
    /// Returns an error if the stream is full.
    pub fn send_from_isr(&self, data: &[u8]) -> Result<usize> {
        let mut bytes_sent: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_send_from_isr(
                self.handle,
                data.as_ptr() as *const _,
                data.len(),
                &mut bytes_sent,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_sent)
    }

    /// Receive bytes from an ISR context (non-blocking, returns immediately).
    ///
    /// Returns the number of bytes received; may be zero if no data is available.
    ///
    /// # Errors
    /// Returns an error if the stream is empty.
    pub fn receive_from_isr(&self, buf: &mut [u8]) -> Result<usize> {
        let mut bytes_received: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_receive_from_isr(
                self.handle,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                &mut bytes_received,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_received)
    }

    /// Reset the stream, discarding all currently buffered data.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    pub fn reset(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_stream_reset(self.handle) };
        Error::from_code(rc)
    }

    /// Return the number of bytes currently available for reading.
    pub fn bytes_available(&self) -> usize {
        unsafe { bindings::ove_stream_bytes_available(self.handle) }
    }
}

crate::ove_handle_impl!(Stream<const N: usize>, ove_stream_destroy, ove_stream_deinit);
