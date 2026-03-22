// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Time and delay utilities for oveRTOS.
//!
//! Provides a monotonic microsecond timestamp ([`get_us`]) and busy-wait or
//! scheduler-yielding delay functions ([`delay_ms`], [`delay_us`]).

use crate::bindings;
use crate::error::{Error, Result};

/// Get the current monotonic time in microseconds since an arbitrary epoch.
///
/// # Errors
/// Returns an error if the underlying hardware timer is unavailable.
pub fn get_us() -> Result<u64> {
    let mut us: u64 = 0;
    let rc = unsafe { bindings::ove_time_get_us(&mut us) };
    Error::from_code(rc)?;
    Ok(us)
}

/// Block the current thread for at least `ms` milliseconds.
///
/// Yields the CPU to other threads for the duration. Prefer [`crate::Thread::sleep_ms`]
/// for thread-level sleeping.
pub fn delay_ms(ms: u32) {
    unsafe { bindings::ove_time_delay_ms(ms) }
}

/// Block the current thread for at least `us` microseconds.
///
/// On most platforms this is a busy-wait for short durations; use sparingly.
pub fn delay_us(us: u32) {
    unsafe { bindings::ove_time_delay_us(us) }
}
