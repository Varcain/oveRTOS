// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! I²S audio bus driver.
//!
//! Safe wrappers around the oveRTOS I²S API for DMA-based audio
//! streaming with double-buffered (ping-pong) operation.
//!
//! Mirrors the [`crate::uart`], [`crate::spi`], and [`crate::i2c`]
//! modules: free functions over an opaque handle.  Lifecycle
//! (`ove_i2s_init` / `ove_i2s_create`) and callback registration
//! (`ove_i2s_set_rx_callback` / `ove_i2s_set_tx_callback`) are left to
//! direct calls against [`crate::ffi`] so the safe surface stays small
//! and the unsafe boundary matches the sibling drivers.
//!
//! # Example
//!
//! ```ignore
//! use ove::ffi;
//! // Heap-mode lifecycle stays at the FFI level — same as UART/SPI/I²C.
//! let mut handle: ffi::ove_i2s_t = core::ptr::null_mut();
//! let cfg = ffi::ove_i2s_cfg { /* ... */ };
//! ove::error::Error::from_code(unsafe { ffi::ove_i2s_create(&mut handle, &cfg) })?;
//!
//! ove::i2s::start(handle)?;
//!
//! // In your RX callback (registered via raw FFI), pull the just-filled half:
//! if let Some(p) = ove::i2s::rx_buf(handle) {
//!     let n = ove::i2s::half_buf_size(handle);
//!     let samples = unsafe { core::slice::from_raw_parts(p, n) };
//!     process(samples);
//! }
//! # Ok::<(), ove::error::Error>(())
//! ```
//!
//! Requires `CONFIG_OVE_I2S`.

use crate::bindings;
use crate::error::{Error, Result};

/// I²S stream direction — mirrors `ove_i2s_dir_t`.
///
/// Pass the underlying `u32` value into `ove_i2s_cfg::direction` via
/// `as bindings::ove_i2s_dir_t`.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Direction {
    /// Transmit only (playback).
    Tx = 0x01,
    /// Receive only (capture).
    Rx = 0x02,
    /// Full-duplex (simultaneous TX + RX).
    TxRx = 0x03,
}

/// Start I²S DMA streaming.
///
/// Begins circular DMA transfers.  TX is started first to generate
/// clocks for a synchronous RX slave when running full-duplex.
#[inline]
pub fn start(i2s: bindings::ove_i2s_t) -> Result<()> {
    Error::from_code(unsafe { bindings::ove_i2s_start(i2s) })
}

/// Stop I²S DMA streaming.
#[inline]
pub fn stop(i2s: bindings::ove_i2s_t) -> Result<()> {
    Error::from_code(unsafe { bindings::ove_i2s_stop(i2s) })
}

/// Pause I²S DMA streaming.  Can be resumed with [`resume`].
#[inline]
pub fn pause(i2s: bindings::ove_i2s_t) -> Result<()> {
    Error::from_code(unsafe { bindings::ove_i2s_pause(i2s) })
}

/// Resume I²S DMA streaming after [`pause`].
#[inline]
pub fn resume(i2s: bindings::ove_i2s_t) -> Result<()> {
    Error::from_code(unsafe { bindings::ove_i2s_resume(i2s) })
}

/// Pointer to the most recently completed RX half-buffer, or `None`
/// if the handle is invalid / no buffer has been filled yet.
///
/// Call from within the RX callback.  The pointed-to memory is
/// `[half_buf_size]` bytes long and remains valid until the next
/// [`stop`] or destroy on the same handle.  To read the samples:
///
/// ```ignore
/// if let Some(p) = ove::i2s::rx_buf(i2s) {
///     let n = ove::i2s::half_buf_size(i2s);
///     let samples = unsafe { core::slice::from_raw_parts(p, n) };
///     /* ... */
/// }
/// ```
#[inline]
pub fn rx_buf(i2s: bindings::ove_i2s_t) -> Option<*const u8> {
    let p = unsafe { bindings::ove_i2s_rx_buf(i2s) };
    if p.is_null() { None } else { Some(p.cast()) }
}

/// Pointer to the TX half-buffer safe to write, or `None` if the
/// handle is invalid.
///
/// Returns the half of the DMA TX buffer that DMA is *not* currently
/// transmitting from.  Fill this buffer before the next TX callback.
/// The pointed-to memory is `[half_buf_size]` bytes long and remains
/// valid until the next [`stop`] or destroy on the same handle.
#[inline]
pub fn tx_buf(i2s: bindings::ove_i2s_t) -> Option<*mut u8> {
    let p = unsafe { bindings::ove_i2s_tx_buf(i2s) };
    if p.is_null() { None } else { Some(p.cast()) }
}

/// Size of one half-buffer in bytes.  Pair with [`rx_buf`] / [`tx_buf`]
/// to construct a slice via `core::slice::from_raw_parts{,_mut}`.
#[inline]
pub fn half_buf_size(i2s: bindings::ove_i2s_t) -> usize {
    unsafe { bindings::ove_i2s_half_buf_size(i2s) }
}
