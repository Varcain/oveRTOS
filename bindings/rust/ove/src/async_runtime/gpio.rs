// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async GPIO input wrapper using the existing `ove_gpio_irq_register`
//! callback path — no new C-side API needed.
//!
//! The pattern differs slightly from stream / queue / sem / eventgroup
//! because GPIO interrupts are edge-triggered events rather than
//! "data became available". We pair an `AtomicWaker` with an
//! `AtomicBool` latch: the IRQ trampoline sets the bool and wakes the
//! waker; the consumer clears the bool on drain. This guards against
//! the edge firing between user-side "register waker" and "go to
//! sleep".

use core::future::poll_fn;
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::Result;
use crate::gpio::{self, GpioIrqMode, GpioPin};

/// Async wrapper around a GPIO input pin.
///
/// Methods take `&'static self` because the C-side `ove_gpio_irq_register`
/// callback retains a pointer to the internal state.
pub struct AsyncInput {
    pin: GpioPin,
    pending: AtomicBool,
    waker: AtomicWaker,
}

// SAFETY: `AsyncInput` holds an `AtomicBool` + `AtomicWaker` (both already
// `Sync`).  No interior thread-bound state; the waker dispatch path uses
// `critical_section` for synchronisation.
unsafe impl Send for AsyncInput {}
unsafe impl Sync for AsyncInput {}

impl AsyncInput {
    /// Wrap a GPIO pin for async edge-event handling.
    pub const fn new(pin: GpioPin) -> Self {
        Self {
            pin,
            pending: AtomicBool::new(false),
            waker: AtomicWaker::new(),
        }
    }

    /// Register the IRQ trampoline and enable interrupts for `mode`
    /// (rising / falling / both). Must be called exactly once.
    pub fn arm(&'static self, mode: GpioIrqMode) -> Result<()> {
        // SAFETY: 'static self; trampoline reinterprets user_data as
        // &'static AsyncInput.
        unsafe {
            gpio::irq_register(
                self.pin,
                mode,
                Some(gpio_irq_trampoline),
                self as *const Self as *mut core::ffi::c_void,
            )?;
        }
        gpio::irq_enable(self.pin)
    }

    /// Await the next edge event. Returns immediately if an edge has
    /// fired since the last call to this function.
    pub async fn wait_for_event(&'static self) {
        poll_fn(|cx| {
            // Fast path: drain any latched edge.
            if self.pending.swap(false, Ordering::AcqRel) {
                return Poll::Ready(());
            }
            self.waker.register(cx.waker());
            // Re-check after register to close the race where the IRQ
            // fires between the first swap and the waker registration.
            if self.pending.swap(false, Ordering::AcqRel) {
                return Poll::Ready(());
            }
            Poll::Pending
        })
        .await
    }

    /// Read the pin's current logic level.
    pub fn is_high(&self) -> Result<bool> {
        gpio::get(self.pin).map(|v| v != 0)
    }

    #[inline]
    pub fn pin(&self) -> GpioPin {
        self.pin
    }
}

unsafe extern "C" fn gpio_irq_trampoline(
    _port: core::ffi::c_uint,
    _pin: core::ffi::c_uint,
    user_data: *mut core::ffi::c_void,
) {
    // SAFETY: user_data was set by AsyncInput::arm to &'static AsyncInput.
    let this = unsafe { &*(user_data as *const AsyncInput) };
    this.pending.store(true, Ordering::Release);
    this.waker.wake();
}
