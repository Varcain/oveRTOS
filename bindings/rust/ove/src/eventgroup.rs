// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Event group (bitfield synchronization) for oveRTOS.
//!
//! An [`EventGroup`] holds a set of bits that any thread or ISR can set or clear.
//! Other threads can block until specific bit patterns appear, enabling fine-grained
//! inter-task signalling without dedicated queues.

use crate::bindings;
use crate::error::{Error, Result};

/// Flags controlling [`EventGroup::wait_bits`] behavior.
///
/// Flags can be combined with the `|` operator.
#[derive(Clone, Copy, Debug, Default)]
pub struct WaitFlags(u32);

impl WaitFlags {
    /// No flags — wait for any bit, don't clear on exit.
    pub const NONE: Self = Self(0);
    /// Wait for ALL specified bits (vs any).
    pub const WAIT_ALL: Self = Self(0x01);
    /// Clear matched bits on successful wait.
    pub const CLEAR_ON_EXIT: Self = Self(0x02);
}

impl core::ops::BitOr for WaitFlags {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self { Self(self.0 | rhs.0) }
}

/// Deprecated alias for [`WaitFlags::WAIT_ALL`].
#[deprecated(note = "use WaitFlags::WAIT_ALL instead")]
pub const EG_WAIT_ALL: WaitFlags = WaitFlags::WAIT_ALL;
/// Deprecated alias for [`WaitFlags::CLEAR_ON_EXIT`].
#[deprecated(note = "use WaitFlags::CLEAR_ON_EXIT instead")]
pub const EG_CLEAR_ON_EXIT: WaitFlags = WaitFlags::CLEAR_ON_EXIT;

/// Event group — a set of named bits that can be set, cleared, and waited on.
pub struct EventGroup {
    handle: bindings::ove_eventgroup_t,
}

impl EventGroup {
    /// Create a new event group via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_eventgroup_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_eventgroup_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `EventGroup`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_eventgroup_storage_t,
    ) -> Result<Self> {
        let mut handle: bindings::ove_eventgroup_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_eventgroup_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Set bits in the event group. Returns the bits value after setting.
    pub fn set_bits(&self, bits: u32) -> u32 {
        unsafe { bindings::ove_eventgroup_set_bits(self.handle, bits) }
    }

    /// Clear bits in the event group. Returns the bits value before clearing.
    pub fn clear_bits(&self, bits: u32) -> u32 {
        unsafe { bindings::ove_eventgroup_clear_bits(self.handle, bits) }
    }

    /// Wait for the specified bits to be set in the event group.
    ///
    /// `flags` is a combination of [`WaitFlags::WAIT_ALL`] and [`WaitFlags::CLEAR_ON_EXIT`].
    /// Returns the bits value at the moment the wait condition was satisfied.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the bits are not set within `timeout_ms`.
    pub fn wait_bits(&self, bits: u32, flags: WaitFlags, timeout_ms: u32) -> Result<u32> {
        let mut result: u32 = 0;
        let rc = unsafe {
            bindings::ove_eventgroup_wait_bits(
                self.handle,
                bits,
                flags.0,
                timeout_ms,
                &mut result,
            )
        };
        Error::from_code(rc)?;
        Ok(result)
    }

    /// Set bits from an ISR context. Returns the bits value after setting.
    pub fn set_bits_from_isr(&self, bits: u32) -> u32 {
        unsafe { bindings::ove_eventgroup_set_bits_from_isr(self.handle, bits) }
    }

    /// Get current bits value.
    pub fn get_bits(&self) -> u32 {
        unsafe { bindings::ove_eventgroup_get_bits(self.handle) }
    }
}

crate::ove_handle_impl!(EventGroup, ove_eventgroup_destroy, ove_eventgroup_deinit);
