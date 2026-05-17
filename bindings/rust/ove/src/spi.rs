// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! SPI bus master driver.
//!
//! Provides safe wrappers around the oveRTOS SPI API with software CS
//! management, thread-safe bus locking, and multi-segment transactions.

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

/// Full-duplex SPI transfer. `tx` or `rx` may be empty for half-duplex.
pub fn transfer(
    spi: bindings::ove_spi_t,
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
    let rc = unsafe { bindings::ove_spi_transfer(spi, cs_ptr, tx_ptr, rx_ptr, len, crate::time::dur_to_ns(timeout)) };
    Error::from_code(rc)
}

/// Write-only SPI transfer.
pub fn write(
    spi: bindings::ove_spi_t,
    cs: Option<&bindings::ove_spi_cs>,
    data: &[u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
    let rc = unsafe {
        bindings::ove_spi_write(spi, cs_ptr, data.as_ptr().cast(), data.len(), crate::time::dur_to_ns(timeout))
    };
    Error::from_code(rc)
}

/// Read-only SPI transfer (clocks out zeros).
pub fn read(
    spi: bindings::ove_spi_t,
    cs: Option<&bindings::ove_spi_cs>,
    buf: &mut [u8],
    timeout: core::time::Duration,
) -> Result<()> {
    let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
    let rc = unsafe {
        bindings::ove_spi_read(spi, cs_ptr, buf.as_mut_ptr().cast(), buf.len(), crate::time::dur_to_ns(timeout))
    };
    Error::from_code(rc)
}

/// Execute a sequence of SPI transfers under a single chip-select assertion.
pub fn transfer_seq(
    spi: bindings::ove_spi_t,
    cs: Option<&bindings::ove_spi_cs>,
    xfers: &[bindings::ove_spi_xfer],
    timeout: core::time::Duration,
) -> Result<()> {
    let cs_ptr = cs.map_or(core::ptr::null(), |c| c as *const _);
    let rc = unsafe {
        bindings::ove_spi_transfer_seq(spi, cs_ptr, xfers.as_ptr(), xfers.len() as u32, crate::time::dur_to_ns(timeout))
    };
    Error::from_code(rc)
}
