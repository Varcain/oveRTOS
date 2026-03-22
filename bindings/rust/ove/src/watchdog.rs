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

impl Drop for Watchdog {
    fn drop(&mut self) {
        if self.handle.is_null() { return; }
        #[cfg(not(zero_heap))]
        unsafe { bindings::ove_watchdog_destroy(self.handle) }
        #[cfg(zero_heap)]
        unsafe { bindings::ove_watchdog_deinit(self.handle) }
    }
}

// SAFETY: Watchdog wraps an opaque RTOS handle. Feed/start are thread-safe
// RTOS calls. Create/destroy are single-threaded (lifecycle guarantee).
unsafe impl Send for Watchdog {}
unsafe impl Sync for Watchdog {}
