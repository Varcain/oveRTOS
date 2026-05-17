// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! I2C bus master driver.
//!
//! Provides a safe wrapper around the oveRTOS I2C API with thread-safe bus
//! locking, register-level convenience functions, and device probing.

use crate::bindings;
use crate::error::{Error, Result};

/// I2C bus speed grade.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Speed {
    /// Standard mode — 100 kHz.
    Standard = 0,
    /// Fast mode — 400 kHz.
    Fast = 1,
    /// Fast-mode Plus — 1 MHz.
    FastPlus = 2,
}

/// Write data to an I2C device.
pub fn write(
    i2c: bindings::ove_i2c_t,
    addr: u16,
    data: &[u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_i2c_write(
            i2c,
            addr,
            data.as_ptr().cast(),
            data.len(),
            crate::time::dur_to_ns(timeout),
        )
    };
    Error::from_code(rc)
}

/// Read data from an I2C device.
pub fn read(
    i2c: bindings::ove_i2c_t,
    addr: u16,
    buf: &mut [u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_i2c_read(
            i2c,
            addr,
            buf.as_mut_ptr().cast(),
            buf.len(),
            crate::time::dur_to_ns(timeout),
        )
    };
    Error::from_code(rc)
}

/// Combined write-then-read with I2C repeated start.
pub fn write_read(
    i2c: bindings::ove_i2c_t,
    addr: u16,
    tx: &[u8],
    rx: &mut [u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_i2c_write_read(
            i2c,
            addr,
            tx.as_ptr().cast(),
            tx.len(),
            rx.as_mut_ptr().cast(),
            rx.len(),
            timeout_ns,
        )
    };
    Error::from_code(rc)
}

/// Write to a single-byte-addressed register.
pub fn reg_write(
    i2c: bindings::ove_i2c_t,
    addr: u16,
    reg: u8,
    data: &[u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_i2c_reg_write(
            i2c,
            addr,
            reg,
            data.as_ptr().cast(),
            data.len(),
            crate::time::dur_to_ns(timeout),
        )
    };
    Error::from_code(rc)
}

/// Read from a single-byte-addressed register.
pub fn reg_read(
    i2c: bindings::ove_i2c_t,
    addr: u16,
    reg: u8,
    buf: &mut [u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_i2c_reg_read(
            i2c,
            addr,
            reg,
            buf.as_mut_ptr().cast(),
            buf.len(),
            timeout_ns,
        )
    };
    Error::from_code(rc)
}

/// Probe for a device at the given address (zero-length write, check ACK).
pub fn probe(i2c: bindings::ove_i2c_t, addr: u16, timeout: core::time::Duration) -> Result<()> {
    let rc = unsafe { bindings::ove_i2c_probe(i2c, addr, crate::time::dur_to_ns(timeout)) };
    Error::from_code(rc)
}
