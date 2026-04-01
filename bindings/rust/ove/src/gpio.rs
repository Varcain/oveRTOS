// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! GPIO control for oveRTOS.
//!
//! Provides pin configuration, digital read/write, and interrupt registration.
//! Pins are identified by a `(port, pin)` pair where both are zero-based indices
//! matching the platform's GPIO numbering.

use crate::bindings;
use crate::error::{Error, Result};

/// GPIO pin direction/pull mode.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum GpioMode {
    /// Pin configured as a digital input.
    Input = 0,
    /// Pin configured as a push-pull output.
    OutputPP = 1,
    /// Pin configured as an open-drain output.
    OutputOD = 2,
}

/// GPIO interrupt trigger mode.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum GpioIrqMode {
    /// Trigger interrupt on rising edge.
    Rising = 0x01,
    /// Trigger interrupt on falling edge.
    Falling = 0x02,
    /// Trigger interrupt on both edges.
    Both = 0x03,
}

/// Configure a GPIO pin's direction and pull mode.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `port` or `pin` is out of range.
pub fn configure(port: u32, pin: u32, mode: GpioMode) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_configure(port, pin, mode as bindings::ove_gpio_mode_t) };
    Error::from_code(rc)
}

/// Set a GPIO pin's output level (`value`: 0 = low, non-zero = high).
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `port` or `pin` is out of range.
pub fn set(port: u32, pin: u32, value: i32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_set(port, pin, value) };
    Error::from_code(rc)
}

/// Read a GPIO pin input value. Returns the pin level (0 or 1) on success.
pub fn get(port: u32, pin: u32) -> Result<i32> {
    let rc = unsafe { bindings::ove_gpio_get(port, pin) };
    if rc < 0 {
        Error::from_code(rc)?;
    }
    Ok(rc)
}

/// Register a GPIO interrupt callback.
///
/// # Safety
/// The callback and user_data must remain valid for the lifetime of the
/// registration.
pub unsafe fn irq_register(
    port: u32,
    pin: u32,
    mode: GpioIrqMode,
    callback: bindings::ove_gpio_irq_cb,
    user_data: *mut core::ffi::c_void,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_gpio_irq_register(port, pin, mode as bindings::ove_gpio_irq_mode_t, callback, user_data)
    };
    Error::from_code(rc)
}

/// Enable a previously registered GPIO interrupt.
///
/// # Errors
/// Returns an error if the interrupt has not been registered or the pin is invalid.
pub fn irq_enable(port: u32, pin: u32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_irq_enable(port, pin) };
    Error::from_code(rc)
}

/// Disable a registered GPIO interrupt without removing the registration.
///
/// # Errors
/// Returns an error if the interrupt has not been registered or the pin is invalid.
pub fn irq_disable(port: u32, pin: u32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_irq_disable(port, pin) };
    Error::from_code(rc)
}
