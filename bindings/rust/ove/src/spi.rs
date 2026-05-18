// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! SPI bus master driver.
//!
//! [`Spi`] wraps an opaque `ove_spi_t` handle and exposes a method-shaped
//! API.  Trait impls (`embedded_hal::spi::SpiBus`, ...) attach in C2.

use crate::bindings;
use crate::error::{Error, Result};

/// SPI clock mode (CPOL/CPHA).
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Mode {
    Mode0 = 0,
    Mode1 = 1,
    Mode2 = 2,
    Mode3 = 3,
}

/// SPI bit order.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum BitOrder {
    MsbFirst = 0,
    LsbFirst = 1,
}

/// SPI bus master.
///
/// Wraps an opaque `ove_spi_t` handle provided by the board configuration.
/// Construct via [`Spi::from_handle`].
#[derive(Debug, Copy, Clone)]
pub struct Spi {
    handle: bindings::ove_spi_t,
}

impl Spi {
    /// Wrap an existing `ove_spi_t` handle.
    ///
    /// # Safety
    /// `handle` must be a valid SPI handle returned by the substrate.
    /// The caller is responsible for ensuring no other `Spi` wrapper
    /// exists for the same handle concurrently.
    #[inline]
    pub const unsafe fn from_handle(handle: bindings::ove_spi_t) -> Self {
        Self { handle }
    }

    /// Return the underlying handle.
    #[inline]
    pub fn raw(&self) -> bindings::ove_spi_t {
        self.handle
    }

    /// Full-duplex SPI transfer. `tx` or `rx` may be empty for half-duplex.
    pub fn transfer(
        &self,
        cs: Option<&bindings::ove_spi_cs>,
        tx: &[u8],
        rx: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let len = tx.len().max(rx.len());
        let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
        let tx_ptr = if tx.is_empty() {
            core::ptr::null()
        } else {
            tx.as_ptr().cast()
        };
        let rx_ptr = if rx.is_empty() {
            core::ptr::null_mut()
        } else {
            rx.as_mut_ptr().cast()
        };
        let rc = unsafe {
            bindings::ove_spi_transfer(
                self.handle,
                cs_ptr,
                tx_ptr,
                rx_ptr,
                len,
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Write-only SPI transfer.
    pub fn write(
        &self,
        cs: Option<&bindings::ove_spi_cs>,
        data: &[u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
        let rc = unsafe {
            bindings::ove_spi_write(
                self.handle,
                cs_ptr,
                data.as_ptr().cast(),
                data.len(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Read-only SPI transfer (clocks out zeros).
    pub fn read(
        &self,
        cs: Option<&bindings::ove_spi_cs>,
        buf: &mut [u8],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
        let rc = unsafe {
            bindings::ove_spi_read(
                self.handle,
                cs_ptr,
                buf.as_mut_ptr().cast(),
                buf.len(),
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }

    /// Execute a sequence of SPI transfers under a single chip-select assertion.
    pub fn transfer_seq(
        &self,
        cs: Option<&bindings::ove_spi_cs>,
        xfers: &[bindings::ove_spi_xfer],
        timeout: core::time::Duration,
    ) -> Result<()> {
        let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
        let rc = unsafe {
            bindings::ove_spi_transfer_seq(
                self.handle,
                cs_ptr,
                xfers.as_ptr(),
                xfers.len() as u32,
                crate::time::dur_to_ns(timeout),
            )
        };
        Error::from_code(rc)
    }
}
