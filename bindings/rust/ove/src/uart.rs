// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! UART serial bus driver.
//!
//! [`Uart`] wraps an opaque `ove_uart_t` handle and exposes a method-shaped
//! API.  `embedded_io::Read` / `embedded_io::Write` impls are provided when
//! the `embedded-io` Cargo feature is enabled.

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

/// UART parity mode.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Parity {
    None = 0,
    Odd = 1,
    Even = 2,
}

/// UART stop bits.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum StopBits {
    One = 0,
    OnePointFive = 1,
    Two = 2,
}

/// UART flow control.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum FlowControl {
    None = 0,
    RtsCts = 1,
}

/// UART driver.
///
/// Wraps an opaque `ove_uart_t` handle provided by the board configuration.
/// Construct via [`Uart::from_handle`].
#[derive(Debug, Copy, Clone)]
pub struct Uart {
    handle: bindings::ove_uart_t,
}

impl Uart {
    /// Wrap an existing `ove_uart_t` handle.
    ///
    /// # Safety
    /// `handle` must be a valid UART handle returned by the substrate.
    /// The caller is responsible for ensuring no other `Uart` wrapper
    /// exists for the same handle concurrently.
    #[inline]
    pub const unsafe fn from_handle(handle: bindings::ove_uart_t) -> Self {
        Self { handle }
    }

    /// Return the underlying handle.
    #[inline]
    pub fn raw(&self) -> bindings::ove_uart_t {
        self.handle
    }

    /// Write data to the UART. Returns the number of bytes written.
    pub fn write(&self, data: &[u8], timeout: core::time::Duration) -> Result<usize> {
        let mut written: usize = 0;
        let rc = unsafe {
            bindings::ove_uart_write(
                self.handle,
                data.as_ptr().cast(),
                data.len(),
                crate::time::dur_to_ns(timeout),
                &mut written,
            )
        };
        Error::from_code(rc)?;
        Ok(written)
    }

    /// Read data from the UART RX buffer. Returns the number of bytes read.
    pub fn read(&self, buf: &mut [u8], timeout: core::time::Duration) -> Result<usize> {
        let mut read_count: usize = 0;
        let rc = unsafe {
            bindings::ove_uart_read(
                self.handle,
                buf.as_mut_ptr().cast(),
                buf.len(),
                crate::time::dur_to_ns(timeout),
                &mut read_count,
            )
        };
        Error::from_code(rc)?;
        Ok(read_count)
    }

    /// Query the number of bytes available in the RX buffer.
    pub fn bytes_available(&self) -> usize {
        unsafe { bindings::ove_uart_bytes_available(self.handle) }
    }

    /// Flush the TX hardware buffer.
    pub fn flush(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_uart_flush(self.handle) };
        Error::from_code(rc)
    }

    /// Register a notify callback fired after every received chunk.
    /// Wraps the C-level `ove_uart_set_rx_notify`, which delegates to
    /// `ove_stream_set_notify` on the UART's internal RX stream.
    ///
    /// # Safety
    /// Same as [`crate::Stream::set_notify`]: `user_data` must outlive
    /// the registration, and `cb` must be ISR-safe (UART RX
    /// typically pushes from ISR context via
    /// `ove_uart_rx_isr_push`).
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_rx_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        let rc = unsafe { bindings::ove_uart_set_rx_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}
