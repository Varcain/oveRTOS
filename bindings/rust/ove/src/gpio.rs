// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! GPIO control for oveRTOS.
//!
//! Provides pin configuration, digital read/write, and interrupt registration.
//! Pins are identified by a [`GpioPin`] which bundles the port and pin number,
//! preventing accidental argument swaps.

use crate::bindings;
use crate::error::{Error, Result};

/// A GPIO pin identified by port and pin number.
///
/// Bundles `(port, pin)` into a single type to prevent accidental argument
/// swaps at the call site.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpioPin {
    /// Zero-based port index.
    pub port: u32,
    /// Zero-based pin index within the port.
    pub pin: u32,
}

impl GpioPin {
    /// Create a new GPIO pin descriptor.
    pub const fn new(port: u32, pin: u32) -> Self {
        Self { port, pin }
    }
}

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
/// Returns [`Error::InvalidParam`] if the pin is out of range.
pub fn configure(pin: GpioPin, mode: GpioMode) -> Result<()> {
    let rc = unsafe {
        bindings::ove_gpio_configure(pin.port, pin.pin, mode as bindings::ove_gpio_mode_t)
    };
    Error::from_code(rc)
}

/// Set a GPIO pin's output level (`value`: 0 = low, non-zero = high).
///
/// # Errors
/// Returns [`Error::InvalidParam`] if the pin is out of range.
pub fn set(pin: GpioPin, value: i32) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_set(pin.port, pin.pin, value) };
    Error::from_code(rc)
}

/// Read a GPIO pin input value. Returns the pin level (0 or 1) on success.
pub fn get(pin: GpioPin) -> Result<i32> {
    let rc = unsafe { bindings::ove_gpio_get(pin.port, pin.pin) };
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
    pin: GpioPin,
    mode: GpioIrqMode,
    callback: bindings::ove_gpio_irq_cb,
    user_data: *mut core::ffi::c_void,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_gpio_irq_register(
            pin.port,
            pin.pin,
            mode as bindings::ove_gpio_irq_mode_t,
            callback,
            user_data,
        )
    };
    Error::from_code(rc)
}

/// Enable a previously registered GPIO interrupt.
///
/// # Errors
/// Returns an error if the interrupt has not been registered or the pin is invalid.
pub fn irq_enable(pin: GpioPin) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_irq_enable(pin.port, pin.pin) };
    Error::from_code(rc)
}

/// Disable a registered GPIO interrupt without removing the registration.
///
/// # Errors
/// Returns an error if the interrupt has not been registered or the pin is invalid.
pub fn irq_disable(pin: GpioPin) -> Result<()> {
    let rc = unsafe { bindings::ove_gpio_irq_disable(pin.port, pin.pin) };
    Error::from_code(rc)
}

// ---------------------------------------------------------------------------
// Fixed-mode pin newtypes — `embedded_hal::digital::{OutputPin, InputPin}`
// trait targets.  Created via constructors that pre-configure the
// underlying pin in the appropriate mode; mode is not changed after
// construction.  For mode-switching use cases stick with the lower-level
// `GpioPin` + free-function API above.
// ---------------------------------------------------------------------------

/// Fixed-mode output GPIO pin.
///
/// Pre-configures the underlying pin as a push-pull or open-drain output
/// at construction time.  Provides simple `set(bool)` access plus the
/// `embedded_hal::digital::OutputPin` trait impl (behind the
/// `embedded-hal` feature).
#[derive(Debug, Clone, Copy)]
pub struct OutputPin {
    pin: GpioPin,
}

impl OutputPin {
    /// Construct a new output pin and configure it.
    ///
    /// # Errors
    /// Returns an error if `mode` isn't an output mode
    /// ([`GpioMode::OutputPP`] or [`GpioMode::OutputOD`]) or the pin is
    /// out of range.
    pub fn new(port: u32, pin: u32, mode: GpioMode) -> Result<Self> {
        if matches!(mode, GpioMode::Input) {
            return Err(Error::InvalidParam);
        }
        let p = GpioPin::new(port, pin);
        configure(p, mode)?;
        Ok(Self { pin: p })
    }

    /// Drive the pin high (`true`) or low (`false`).
    #[inline]
    pub fn set(&mut self, value: bool) -> Result<()> {
        set(self.pin, if value { 1 } else { 0 })
    }

    /// Drive the pin low.
    #[inline]
    pub fn set_low(&mut self) -> Result<()> {
        set(self.pin, 0)
    }

    /// Drive the pin high.
    #[inline]
    pub fn set_high(&mut self) -> Result<()> {
        set(self.pin, 1)
    }

    /// Return the underlying `GpioPin` (`(port, pin)`).
    #[inline]
    pub fn pin(&self) -> GpioPin {
        self.pin
    }
}

/// Fixed-mode input GPIO pin.
///
/// Pre-configures the underlying pin as a digital input at construction
/// time.  Provides `is_high()` / `is_low()` access plus the
/// `embedded_hal::digital::InputPin` trait impl (behind the
/// `embedded-hal` feature).
#[derive(Debug, Clone, Copy)]
pub struct InputPin {
    pin: GpioPin,
}

impl InputPin {
    /// Construct a new input pin and configure it as a digital input.
    pub fn new(port: u32, pin: u32) -> Result<Self> {
        let p = GpioPin::new(port, pin);
        configure(p, GpioMode::Input)?;
        Ok(Self { pin: p })
    }

    /// Read the pin level — returns `true` if high.
    #[inline]
    pub fn is_high(&self) -> Result<bool> {
        Ok(get(self.pin)? != 0)
    }

    /// Read the pin level — returns `true` if low.
    #[inline]
    pub fn is_low(&self) -> Result<bool> {
        Ok(!self.is_high()?)
    }

    /// Return the underlying `GpioPin` (`(port, pin)`).
    #[inline]
    pub fn pin(&self) -> GpioPin {
        self.pin
    }
}
