// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Backward-compatible BSP module — delegates to board, gpio, led modules.

use crate::bindings;
use crate::error::{Error, Result};

/// Initialize the board support package.
///
/// # Errors
/// Returns an error if any hardware peripheral fails to initialize.
pub fn board_init() -> Result<()> {
    let rc = unsafe { bindings::ove_board_init() };
    Error::from_code(rc)
}

/// Toggle the state of an LED.
pub fn led_toggle(led: u32) {
    unsafe { bindings::ove_led_toggle(led) }
}

/// Set the state of an LED.
pub fn led_set(led: u32, on: bool) {
    unsafe { bindings::ove_led_set(led, on as i32) }
}

/// Set a GPIO pin's output level (`value`: 0 = low, non-zero = high).
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `port` or `pin` is out of range.
pub fn gpio_set(port: u32, pin: u32, value: i32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_set(port, pin, value) };
    Error::from_code(rc)
}

/// Read a GPIO pin input value. Returns 0 (low) or 1 (high) on success.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `port` or `pin` is out of range.
pub fn gpio_get(port: u32, pin: u32) -> Result<i32> {
    let rc = unsafe { bindings::ove_gpio_get(port, pin) };
    if rc < 0 {
        Error::from_code(rc)?;
    }
    Ok(rc)
}

/// GPIO interrupt trigger mode (BSP re-export of [`gpio::GpioIrqMode`](crate::gpio::GpioIrqMode)).
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum GpioIrqMode {
    /// Trigger on rising edge.
    Rising = 0x01,
    /// Trigger on falling edge.
    Falling = 0x02,
    /// Trigger on both edges.
    Both = 0x03,
}

/// Register a GPIO interrupt callback.
///
/// # Safety
/// The callback and user_data must remain valid for the lifetime of the
/// registration.
pub unsafe fn gpio_irq_register(
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
pub fn gpio_irq_enable(port: u32, pin: u32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_irq_enable(port, pin) };
    Error::from_code(rc)
}

/// Disable a registered GPIO interrupt without removing the registration.
///
/// # Errors
/// Returns an error if the interrupt has not been registered or the pin is invalid.
pub fn gpio_irq_disable(port: u32, pin: u32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_irq_disable(port, pin) };
    Error::from_code(rc)
}
