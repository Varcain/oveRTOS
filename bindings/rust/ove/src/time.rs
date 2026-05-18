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

/// Zero-sized handle used as the trait target for
/// `embedded_hal::delay::DelayNs` (behind the `embedded-hal` feature).
///
/// Routes `delay_*` calls into the substrate's [`delay_us`] / [`delay_ms`].
/// Sub-microsecond delays round up to one microsecond — the substrate
/// lacks nanosecond resolution, and most embedded-hal driver use cases
/// (protocol setup/hold times) tolerate the round-up.
#[derive(Debug, Clone, Copy, Default)]
pub struct Delay;

/// Get the current monotonic time in nanoseconds since an arbitrary epoch.
///
/// Prefer [`Instant::now`] for new code — it's the typed counterpart and
/// the only constructor for [`Instant`] outside of `FOREVER` / `Add<Duration>`
/// composition.  This bare-`u64` helper is kept as an escape hatch.
#[inline]
pub fn now_steady_ns() -> u64 {
    let mut ns: u64 = 0;
    unsafe { bindings::ove_time_get_ns(&mut ns) };
    ns
}

/// Typed monotonic timestamp for `try_*_until` deadlines.
///
/// Wraps a nanosecond count from the substrate's steady clock.  Construct
/// only via [`Instant::now`] or by adding a [`Duration`] to an existing
/// `Instant`.  The internal representation is opaque on purpose: this is
/// what makes a raw `u64` of microseconds or relative duration nanoseconds
/// fail to compile when fed to a `try_*_until` call.
///
/// Matches `std::time::Instant`'s opaque-newtype shape (we can't use the
/// std type directly because it lives in `std`, not `core` — see also
/// `parking_lot::Mutex::try_lock_until` which takes std `Instant` on
/// host but the equivalent type on `no_std` targets).
///
/// # Examples
///
/// ```ignore
/// use core::time::Duration;
/// use ove::time::Instant;
///
/// let deadline = Instant::now() + Duration::from_millis(100);
/// mtx.try_lock_until(deadline)?;
///
/// // Or wait indefinitely:
/// mtx.try_lock_until(Instant::FOREVER)?;
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Instant(u64);

impl Instant {
    /// Returns the current value of the substrate steady clock.
    #[inline]
    pub fn now() -> Self {
        Self(now_steady_ns())
    }

    /// Sentinel "wait indefinitely" deadline.
    ///
    /// Mapped to the substrate's `OVE_WAIT_FOREVER` constant via the
    /// internal raw-ns representation.  Useful when a function's
    /// signature only exposes a deadline variant but the caller wants
    /// to block forever.
    pub const FOREVER: Instant = Instant(u64::MAX);

    /// Raw nanosecond representation — used internally to bridge into
    /// the substrate's `deadline_ns: uint64_t` parameter.
    #[inline]
    pub(crate) fn raw(self) -> u64 {
        self.0
    }
}

impl core::ops::Add<Duration> for Instant {
    type Output = Instant;
    #[inline]
    fn add(self, rhs: Duration) -> Instant {
        let bumped = self.0.saturating_add(dur_to_ns(rhs));
        Instant(bumped)
    }
}

impl core::ops::AddAssign<Duration> for Instant {
    #[inline]
    fn add_assign(&mut self, rhs: Duration) {
        *self = *self + rhs;
    }
}

impl core::ops::Sub<Instant> for Instant {
    type Output = Duration;
    #[inline]
    fn sub(self, rhs: Instant) -> Duration {
        Duration::from_nanos(self.0.saturating_sub(rhs.0))
    }
}

/// Convert an [`Instant`] deadline to the timeout-ns value the substrate
/// expects (`OVE_WAIT_FOREVER` is preserved; otherwise `deadline - now`,
/// saturating to 0 if the deadline is in the past).
#[inline]
pub(crate) fn deadline_to_timeout_ns(deadline: Instant) -> u64 {
    if deadline.0 == u64::MAX {
        return u64::MAX;
    }
    let now = now_steady_ns();
    deadline.0.saturating_sub(now)
}
