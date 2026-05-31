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

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;

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
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

/// Deprecated alias for [`WaitFlags::WAIT_ALL`].
#[deprecated(note = "use WaitFlags::WAIT_ALL instead")]
pub const EG_WAIT_ALL: WaitFlags = WaitFlags::WAIT_ALL;
/// Deprecated alias for [`WaitFlags::CLEAR_ON_EXIT`].
#[deprecated(note = "use WaitFlags::CLEAR_ON_EXIT instead")]
pub const EG_CLEAR_ON_EXIT: WaitFlags = WaitFlags::CLEAR_ON_EXIT;

/// Caller-owned storage for an [`EventGroup`] in zero-heap mode (see
/// [`crate::MutexStorage`]).
#[allow(dead_code)]
pub struct EventGroupStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_eventgroup_storage_t>>,
}

impl EventGroupStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for EventGroupStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see crate::MutexStorage.
unsafe impl Sync for EventGroupStorage {}

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
    pub unsafe fn from_static(storage: *mut bindings::ove_eventgroup_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_eventgroup_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_eventgroup_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Mode-agnostic constructor (see [`crate::Mutex::create`]).
    pub fn create(storage: &'static EventGroupStorage) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new()
        }
        #[cfg(zero_heap)]
        {
            let ptr = UnsafeCell::raw_get(&storage.storage).cast();
            unsafe { Self::from_static(ptr) }
        }
    }

    /// Set bits in the event group. Returns the bits value after setting.
    #[inline]
    pub fn set_bits(&self, bits: u32) -> u32 {
        unsafe { bindings::ove_eventgroup_set_bits(self.handle, bits) }
    }

    /// Clear bits in the event group. Returns the bits value before clearing.
    #[inline]
    pub fn clear_bits(&self, bits: u32) -> u32 {
        unsafe { bindings::ove_eventgroup_clear_bits(self.handle, bits) }
    }

    /// Wait indefinitely for the specified bits to be set.  Returns
    /// the bits value at the moment the wait condition was satisfied.
    ///
    /// `flags` is a combination of [`WaitFlags::WAIT_ALL`] and
    /// [`WaitFlags::CLEAR_ON_EXIT`].
    #[inline]
    pub fn wait_bits(&self, bits: u32, flags: WaitFlags) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, u64::MAX)
    }

    /// Non-blocking check for the specified bits.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the bits are not currently set.
    #[inline]
    pub fn try_wait_bits(&self, bits: u32, flags: WaitFlags) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, 0)
    }

    /// Wait up to `d` for the specified bits.
    #[inline]
    pub fn wait_bits_for(
        &self,
        bits: u32,
        flags: WaitFlags,
        d: core::time::Duration,
    ) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, crate::time::dur_to_ns(d))
    }

    /// Wait by the given deadline.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    #[inline]
    pub fn wait_bits_until(
        &self,
        bits: u32,
        flags: WaitFlags,
        deadline: crate::time::Instant,
    ) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, crate::time::deadline_to_timeout_ns(deadline))
    }

    #[inline]
    fn wait_bits_with_timeout(&self, bits: u32, flags: WaitFlags, timeout_ns: u64) -> Result<u32> {
        let mut result: u32 = 0;
        let rc = unsafe {
            bindings::ove_eventgroup_wait_bits(self.handle, bits, flags.0, timeout_ns, &mut result)
        };
        Error::from_code(rc)?;
        Ok(result)
    }

    /// Set bits from an ISR context. Returns the bits value after setting.
    #[inline]
    pub fn set_bits_from_isr(&self, bits: u32) -> u32 {
        unsafe { bindings::ove_eventgroup_set_bits_from_isr(self.handle, bits) }
    }

    /// Get current bits value.
    #[inline]
    pub fn get_bits(&self) -> u32 {
        unsafe { bindings::ove_eventgroup_get_bits(self.handle) }
    }

    /// Register a notify callback fired after every successful set_bits.
    /// Wraps the C-level `ove_eventgroup_set_notify`.
    ///
    /// # Safety
    /// Same as [`crate::Stream::set_notify`]: `user_data` must remain
    /// valid for as long as the callback may fire, and `cb` must be
    /// ISR-safe (the C-side invokes it from whatever context the set
    /// ran in, thread or ISR).
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        let rc = unsafe { bindings::ove_eventgroup_set_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}

crate::ove_handle_impl!(EventGroup, ove_eventgroup_destroy, ove_eventgroup_deinit);
