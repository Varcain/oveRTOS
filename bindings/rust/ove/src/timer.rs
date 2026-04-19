// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Software timer for oveRTOS.
//!
//! [`Timer`] wraps an RTOS software timer with a safe Rust `fn()` callback.
//! Timers can be periodic or one-shot and are driven by the RTOS tick.

use crate::bindings;
use crate::error::{Error, Result};

/// Software timer with a safe Rust callback.
///
/// The timer stores a function pointer and generates an internal trampoline
/// so the user callback is a plain `fn()` — no `unsafe`, no raw pointers.
pub struct Timer {
    handle: bindings::ove_timer_t,
}

impl Timer {
    /// Create a new timer via heap allocation (only in heap mode).
    ///
    /// - `callback` — safe Rust function called each time the timer fires.
    /// - `period_ms` — timer period in milliseconds.
    /// - `one_shot` — if `true`, the timer fires once and stops.
    #[cfg(not(zero_heap))]
    pub fn new(callback: fn(), period_ms: u32, one_shot: bool) -> Result<Self> {
        // Store the callback as the user_data pointer. On platforms where
        // fn() is pointer-sized this is a direct cast; the trampoline
        // reconverts it.
        let user_data = callback as *mut core::ffi::c_void;

        let mut handle: bindings::ove_timer_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_timer_create(
                &mut handle,
                Some(Self::trampoline),
                user_data,
                period_ms,
                one_shot as i32,
            )
        };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Timer`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_timer_storage_t,
        callback: fn(),
        period_ms: u32,
        one_shot: bool,
    ) -> Result<Self> {
        let user_data = callback as *mut core::ffi::c_void;
        let mut handle: bindings::ove_timer_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_timer_init(
                &mut handle,
                storage,
                Some(Self::trampoline),
                user_data,
                period_ms,
                one_shot as i32,
            )
        };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Start the timer, beginning countdown from now.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    pub fn start(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_timer_start(self.handle) };
        Error::from_code(rc)
    }

    /// Stop the timer, preventing further callbacks until restarted.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    pub fn stop(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_timer_stop(self.handle) };
        Error::from_code(rc)
    }

    /// Reset the timer, restarting the period from now.
    ///
    /// If the timer is stopped, this also starts it.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    pub fn reset(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_timer_reset(self.handle) };
        Error::from_code(rc)
    }

    /// Internal trampoline that converts the C callback into a safe Rust call.
    unsafe extern "C" fn trampoline(
        _timer: bindings::ove_timer_t,
        user_data: *mut core::ffi::c_void,
    ) {
        // SAFETY: `user_data` was stored by `Timer::new`/`from_static` from a
        // `fn()` pointer. Targets supported by oveRTOS have pointer-sized
        // function pointers with a C-compatible ABI, so round-tripping
        // through `*mut c_void` is well-defined.
        let cb: fn() = unsafe { core::mem::transmute(user_data) };
        cb();
    }
}

crate::ove_handle_impl!(Timer, ove_timer_destroy, ove_timer_deinit);
