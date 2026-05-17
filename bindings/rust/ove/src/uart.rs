// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! UART serial bus driver.
//!
//! Provides safe wrappers around the oveRTOS UART API with interrupt-driven
//! RX buffering and thread-safe TX.

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

/// Write data to the UART. Returns the number of bytes written.
pub fn write(
    uart: bindings::ove_uart_t,
    data: &[u8],
    timeout: core::time::Duration,
) -> Result<usize> {
    let mut written: usize = 0;
    let rc = unsafe {
        bindings::ove_uart_write(
            uart,
            data.as_ptr().cast(),
            data.len(),
            timeout_ns,
            &mut written,
        )
    };
    Error::from_code(rc)?;
    Ok(written)
}

/// Read data from the UART RX buffer. Returns the number of bytes read.
pub fn read(
    uart: bindings::ove_uart_t,
    buf: &mut [u8],
    timeout: core::time::Duration,
) -> Result<usize> {
    let mut read_count: usize = 0;
    let rc = unsafe {
        bindings::ove_uart_read(
            uart,
            buf.as_mut_ptr().cast(),
            buf.len(),
            timeout_ns,
            &mut read_count,
        )
    };
    Error::from_code(rc)?;
    Ok(read_count)
}

/// Query the number of bytes available in the RX buffer.
pub fn bytes_available(uart: bindings::ove_uart_t) -> usize {
    unsafe { bindings::ove_uart_bytes_available(uart) }
}

/// Flush the TX hardware buffer.
pub fn flush(uart: bindings::ove_uart_t) -> Result<()> {
    let rc = unsafe { bindings::ove_uart_flush(uart) };
    Error::from_code(rc)
}
