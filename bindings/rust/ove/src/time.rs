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
use core::time::Duration;

/// Convert a [`Duration`] to `u64` nanoseconds for the C ABI.
///
/// Saturates to `u64::MAX` if the duration overflows `u64` ns
/// (~584 years). Used internally by every binding wrapper that
/// passes a timeout to a substrate function taking `uint64_t timeout_ns`.
#[inline]
pub(crate) fn dur_to_ns(d: Duration) -> u64 {
    let n = d.as_nanos();
    if n > u64::MAX as u128 {
        u64::MAX
    } else {
        n as u64
    }
}

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

/// Get the current monotonic time in nanoseconds since an arbitrary epoch.
///
/// The epoch matches the substrate's steady clock; use with `_until`
/// variants to compose a deadline:
/// ```ignore
/// let deadline_ns = ove::time::now_steady_ns() + 100_000_000;
/// mtx.lock_until(deadline_ns)?;
/// ```
#[inline]
pub fn now_steady_ns() -> u64 {
    let mut ns: u64 = 0;
    unsafe { bindings::ove_time_get_ns(&mut ns) };
    ns
}

/// Convert an absolute steady-clock deadline to the time remaining,
/// preserving the `u64::MAX` "wait forever" sentinel.  Returns 0 when
/// the deadline is in the past.
///
/// Used internally by every binding wrapper's `_until` variant.  The
/// substrate exposes the same helper as `ove_time_deadline_to_timeout_ns`
/// (a `static inline` in `<ove/time.h>` that bindgen can't reach).
#[inline]
pub(crate) fn deadline_to_timeout_ns(deadline_ns: u64) -> u64 {
    if deadline_ns == u64::MAX {
        return u64::MAX;
    }
    let now = now_steady_ns();
    if deadline_ns > now {
        deadline_ns - now
    } else {
        0
    }
}

