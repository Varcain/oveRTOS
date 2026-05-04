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
#[inline]
pub fn get_us() -> Result<u64> {
    let mut us: u64 = 0;
    let rc = unsafe { bindings::ove_time_get_us(&mut us) };
    Error::from_code(rc)?;
    Ok(us)
}

/// Like [`get_us`] but skips the error-mapping branch — the underlying
/// `ove_time_get_us` is infallible on every supported backend's
/// hardware timer (the `Result` exists only to keep the API uniform
/// with other ove fallible operations).
///
/// Use this when calling `get_us` in tight hot loops where the
/// `Result<u64>` plumbing dominates the actual measurement (the bench
/// suite's `time_get_us_overhead` case is the canonical example).
#[inline]
pub fn get_us_unchecked() -> u64 {
    let mut us: u64 = 0;
    unsafe { bindings::ove_time_get_us(&mut us) };
    us
}

/// Block the current thread for at least `ms` milliseconds.
///
/// Yields the CPU to other threads for the duration. Prefer [`crate::Thread::sleep_ms`]
/// for thread-level sleeping.
#[inline]
pub fn delay_ms(ms: u32) {
    unsafe { bindings::ove_time_delay_ms(ms) }
}

/// Block the current thread for at least `us` microseconds.
///
/// On most platforms this is a busy-wait for short durations; use sparingly.
#[inline]
pub fn delay_us(us: u32) {
    unsafe { bindings::ove_time_delay_us(us) }
}
