// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! I2C bus master driver.
//!
//! [`I2c`] wraps an opaque `ove_i2c_t` handle and exposes a method-shaped
//! API.  An `embedded_hal::i2c::I2c` impl is provided when the
//! `embedded-hal` Cargo feature is enabled.

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

/// I2C bus master.
///
/// Wraps an opaque `ove_i2c_t` handle provided by the board configuration
/// (the substrate manages the handle's lifetime; the binding does not own
/// it).  Construct via [`I2c::from_handle`] with a handle obtained from
/// the BSP layer or board descriptor.
#[derive(Debug, Copy, Clone)]
pub struct I2c {
    handle: bindings::ove_i2c_t,
}

impl I2c {
    /// Wrap an existing `ove_i2c_t` handle.
    ///
    /// # Safety
    /// - `handle` must be a valid I2C handle returned by the substrate
    ///   (typically via the board config / BSP layer).
    /// - The caller is responsible for ensuring no other `I2c` wrapper
    ///   exists for the same handle concurrently — the substrate
    ///   serialises bus access internally, but constructing two
    ///   wrappers with conflicting trait-impl state is a logic error.
    #[inline]
    pub const unsafe fn from_handle(handle: bindings::ove_i2c_t) -> Self {
        Self { handle }
    }

    /// Return the underlying handle.  Useful when bridging into the
    /// raw FFI for an operation the binding hasn't surfaced.
    #[inline]
    pub fn raw(&self) -> bindings::ove_i2c_t {
        self.handle
    }

    /// Write data to an I2C device.
    pub fn write(&self, addr: u16, data: &[u8], timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_write(
                self.handle,
                addr,
                data.as_ptr().cast(),
                data.len(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Read data from an I2C device.
    pub fn read(&self, addr: u16, buf: &mut [u8], timeout: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_read(
                self.handle,
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
        &self,
        addr: u16,
        tx: &[u8],
        rx: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_write_read(
                self.handle,
                addr,
                tx.as_ptr().cast(),
                tx.len(),
                rx.as_mut_ptr().cast(),
                rx.len(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Write to a single-byte-addressed register.
    pub fn reg_write(
        &self,
        addr: u16,
        reg: u8,
        data: &[u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_reg_write(
                self.handle,
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
        &self,
        addr: u16,
        reg: u8,
        buf: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let rc = unsafe {
            bindings::ove_i2c_reg_read(
                self.handle,
                addr,
                reg,
                buf.as_mut_ptr().cast(),
                buf.len(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Probe for a device at the given address (zero-length write, check ACK).
    pub fn probe(&self, addr: u16, timeout: core::time::Duration) -> Result<()> {
        let rc =
            unsafe { bindings::ove_i2c_probe(self.handle, addr, crate::time::dur_to_ns(timeout)) };
        Error::from_code(rc)
    }
}
