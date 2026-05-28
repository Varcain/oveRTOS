// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Hardware watchdog timer for oveRTOS.
//!
//! A [`Watchdog`] triggers a system reset if its countdown is not refreshed by
//! calling [`Watchdog::feed`] within the configured timeout. Start with
//! [`Watchdog::start`] and call `feed` periodically from a health-check thread.

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY (module-wide contract for the `unsafe { bindings::ove_*(...) }` FFI
// calls below): any handle passed to the C API is non-null and refers to a
// live RTOS object — wrapper constructors establish validity via
// `Error::from_code`, and `Drop` (or an explicit `deinit`) is the only place
// a handle is released. Pointer and slice arguments reference caller-owned
// memory valid for the duration of the call; the C side copies whatever it
// retains and does not alias them past return (verified against the
// signatures in `include/ove/*.h`). Blocks that deviate — `transmute`, raw
// pointer casts from user data, slice reconstruction via `from_raw_parts`,
// or storing a callback across the FFI boundary — carry their own
// `// SAFETY:` comment.

/// RAII watchdog timer.
pub struct Watchdog {
    handle: bindings::ove_watchdog_t,
}

impl Watchdog {
    /// Create a new watchdog via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new(timeout_ms: u32) -> Result<Self> {
        let mut handle: bindings::ove_watchdog_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_watchdog_create(&mut handle, timeout_ms) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Watchdog`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_watchdog_storage_t,
        timeout_ms: u32,
    ) -> Result<Self> {
        let mut handle: bindings::ove_watchdog_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_watchdog_init(&mut handle, storage, timeout_ms) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Start the watchdog timer. The system will reset if [`feed`](Watchdog::feed)
    /// is not called within `timeout_ms` milliseconds.
    ///
    /// # Errors
    /// Returns an error if the watchdog hardware could not be started.
    pub fn start(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_watchdog_start(self.handle) };
        Error::from_code(rc)
    }

    /// Feed (kick) the watchdog to reset its countdown and prevent a system reset.
    ///
    /// # Errors
    /// Returns an error if the watchdog hardware rejects the feed command.
    pub fn feed(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_watchdog_feed(self.handle) };
        Error::from_code(rc)
    }
}

crate::ove_handle_impl!(Watchdog, ove_watchdog_destroy, ove_watchdog_deinit);
