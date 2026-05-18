// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! UART serial bus driver.
//!
//! [`Uart`] wraps an opaque `ove_uart_t` handle and exposes a method-shaped
//! API.  Trait impls (`embedded_io::Read`, `embedded_io::Write`, ...) attach
//! in C3.

use crate::bindings;
use crate::error::{Error, Result};

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
}
