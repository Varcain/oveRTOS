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
use core::fmt;
use core::mem::MaybeUninit;
use core::sync::atomic::AtomicBool;
#[cfg(zero_heap)]
use core::sync::atomic::Ordering;

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY: successful construction publishes an initialized nonnull handle.
// One owner keeps its native backing live through complete admitted cleanup;
// synchronous operations borrow that owner through their callback tails.
// Native deferred commands detach from backing before cleanup returns.
// Raw backing and retained notification data have additional unsafe contracts.

fn resource_context() -> bool {
    // SAFETY: pointer-free, allocation-free query available with EventGroup alone.
    (unsafe { bindings::ove_sync_context() } & bindings::OVE_SYNC_CONTEXT_RESOURCE) != 0
}

fn require_resource_context() -> Result<()> {
    if resource_context() {
        Ok(())
    } else {
        Err(Error::NotSupported)
    }
}

fn blocking_context() -> Result<()> {
    // SAFETY: pointer-free query before any clock or primitive operation.
    if (unsafe { bindings::ove_sync_context() } & bindings::OVE_SYNC_CONTEXT_BLOCKING) != 0 {
        Ok(())
    } else {
        Err(Error::NotSupported)
    }
}

#[derive(Clone, Copy)]
enum EventGroupOrigin {
    #[cfg(not(zero_heap))]
    Heap,
    #[cfg(zero_heap)]
    Static(&'static AtomicBool),
    #[cfg(zero_heap)]
    Raw,
}

impl EventGroupOrigin {
    #[cfg_attr(
        not(zero_heap),
        allow(clippy::unused_self, reason = "heap owners have no static claim")
    )]
    fn release_claim(self) {
        #[cfg(zero_heap)]
        if let Self::Static(claim) = self {
            claim.store(false, Ordering::Release);
        }
    }
}

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

/// Caller-owned storage for an [`EventGroup`] in zero-heap mode.
/// Declare in a static and pass a shared reference to [`EventGroup::create`].
/// A live, initializing, tearing-down, forgotten or retained owner keeps its
/// claim closed until complete legal native cleanup. Heap mode ignores it.
#[allow(dead_code)]
pub struct EventGroupStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_eventgroup_storage_t>>,
    claimed: AtomicBool,
}

impl EventGroupStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }
}

impl Default for EventGroupStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: private bytes expose no references. A unique atomic claimant owns
// native initialization/access/cleanup; forget and unsupported Drop retain it.
unsafe impl Sync for EventGroupStorage {}

/// Event group — a set of named bits that can be set, cleared, and waited on.
///
/// Construction, cleanup and notification setup require RESOURCE permission.
/// Unsupported Drop retains the native resource and its static claim.
/// All task operations, including zero-timeout waits, require BLOCKING.
/// Unsupported scalar methods return inert zero; Result methods return
/// [`Error::NotSupported`] before primitive or deadline-clock effects.
pub struct EventGroup {
    handle: bindings::ove_eventgroup_t,
    origin: EventGroupOrigin,
}

impl EventGroup {
    /// Configured usable payload bits, without accessing a native object.
    /// Use this mask for clear-all or wait-for-any-bit operations. FreeRTOS
    /// reserves its high native byte; the other engines expose all 32 bits.
    #[must_use]
    pub fn valid_mask() -> u32 {
        // SAFETY: this pointer-free query only returns a configured mask.
        unsafe { bindings::ove_eventgroup_valid_mask() }
    }

    /// Create a new event group via heap allocation (only in heap mode).
    /// Native initialization failures propagate without publishing a handle;
    /// successfully initialized native resources and the allocation are released.
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        require_resource_context()?;
        let mut handle: bindings::ove_eventgroup_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_eventgroup_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: EventGroupOrigin::Heap,
        })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Storage must be nonnull, aligned, complete, writable, unused and stable
    /// through actual native cleanup or indefinite retention after forget or
    /// unsupported Drop. No aliased live Rust reference into the native extent,
    /// competing initialization, teardown or raw/wrapper access is permitted.
    /// Native context, operation and external producer quiescence contracts
    /// still apply. This entry does not own or reopen a typed storage claim.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_eventgroup_storage_t) -> Result<Self> {
        require_resource_context()?;
        let mut handle: bindings::ove_eventgroup_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_eventgroup_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: EventGroupOrigin::Raw,
        })
    }

    /// Mode-agnostic constructor. Heap mode ignores storage. Zero-heap mode
    /// claims it before native initialization, returning [`Error::Busy`] while
    /// another generation is live or retained. Clean errors release the claim.
    pub fn create(storage: &'static EventGroupStorage) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new()
        }
        #[cfg(zero_heap)]
        {
            require_resource_context()?;
            storage
                .claimed
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .map_err(|_| Error::Busy)?;
            let ptr = UnsafeCell::raw_get(&storage.storage).cast();
            // SAFETY: a sole claim owns complete, aligned, stable typed backing.
            match unsafe { Self::from_static(ptr) } {
                Ok(mut owner) => {
                    owner.origin = EventGroupOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }

    /// Set bits in the event group. Returns the post-set value.
    /// Bits outside [`Self::valid_mask`] return inert zero without effects;
    /// zero is also a possible genuine snapshot. Setting zero does not notify.
    /// Returns inert zero without native effects when BLOCKING is unavailable.
    #[inline]
    pub fn set_bits(&self, bits: u32) -> u32 {
        if blocking_context().is_err() {
            return 0;
        }
        unsafe { bindings::ove_eventgroup_set_bits(self.handle, bits) }
    }

    /// Clear bits and return the full prior snapshot atomically.
    /// Bits outside [`Self::valid_mask`] return inert zero without effects.
    /// Clearing zero returns the unchanged prior snapshot.
    /// Returns inert zero without native effects when BLOCKING is unavailable.
    #[inline]
    pub fn clear_bits(&self, bits: u32) -> u32 {
        if blocking_context().is_err() {
            return 0;
        }
        unsafe { bindings::ove_eventgroup_clear_bits(self.handle, bits) }
    }

    /// Wait indefinitely for the specified bits to be set.  Returns
    /// the bits value at the moment the wait condition was satisfied.
    ///
    /// `flags` is a combination of [`WaitFlags::WAIT_ALL`] and
    /// [`WaitFlags::CLEAR_ON_EXIT`].
    /// Zero masks or bits outside [`Self::valid_mask`] return
    /// [`Error::InvalidParam`] without native wait effects.
    #[inline]
    pub fn wait_bits(&self, bits: u32, flags: WaitFlags) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, u64::MAX)
    }

    /// Non-blocking check for the specified bits.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the bits are not currently set, or
    /// [`Error::InvalidParam`] for zero/invalid masks or unknown flags.
    #[inline]
    pub fn try_wait_bits(&self, bits: u32, flags: WaitFlags) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, 0)
    }

    /// Wait up to `d` for the specified bits.
    /// Zero masks or bits outside [`Self::valid_mask`] return
    /// [`Error::InvalidParam`] without native wait effects.
    #[inline]
    pub fn wait_bits_for(
        &self,
        bits: u32,
        flags: WaitFlags,
        d: core::time::Duration,
    ) -> Result<u32> {
        self.wait_bits_with_timeout(bits, flags, crate::duration::dur_to_ns(d))
    }

    /// Wait by the given deadline.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    /// Invalid masks return [`Error::InvalidParam`] before reading the clock.
    #[inline]
    #[cfg(has_time)]
    pub fn wait_bits_until(
        &self,
        bits: u32,
        flags: WaitFlags,
        deadline: crate::time::Instant,
    ) -> Result<u32> {
        blocking_context()?;
        if bits == 0 || bits & !Self::valid_mask() != 0 || flags.0 & !3 != 0 {
            return Err(Error::InvalidParam);
        }
        self.wait_bits_with_timeout(bits, flags, crate::time::deadline_to_timeout_ns(deadline))
    }

    #[inline]
    fn wait_bits_with_timeout(&self, bits: u32, flags: WaitFlags, timeout_ns: u64) -> Result<u32> {
        blocking_context()?;
        let mut result: u32 = 0;
        let rc = unsafe {
            bindings::ove_eventgroup_wait_bits(self.handle, bits, flags.0, timeout_ns, &mut result)
        };
        Error::from_code(rc)?;
        Ok(result)
    }

    /// Set bits from an ISR context. Actual FreeRTOS interrupts return the accepted requested
    /// mask, or zero on queue failure; publication is deferred. Other engines
    /// return the post-set snapshot. Invalid bits return inert zero.
    /// Task callers use [`Self::set_bits`], including its native task snapshot
    /// and context fallback. Interrupt callers must satisfy native kernel-aware
    /// priority/setup rules; detection does not qualify NMI or arbitrary IRQs.
    #[inline]
    pub fn set_bits_from_isr(&self, bits: u32) -> u32 {
        // SAFETY: pointer-free detection does not access the native object.
        if !unsafe { bindings::ove_is_in_isr() } {
            return self.set_bits(bits);
        }
        unsafe { bindings::ove_eventgroup_set_bits_from_isr(self.handle, bits) }
    }

    /// Clear bits from an ISR context. On FreeRTOS the clear is deferred to
    /// the timer service task; [`Error::QueueFull`] means it could not be
    /// queued. Bits outside [`Self::valid_mask`] return [`Error::InvalidParam`].
    /// Task callers use the admitted task clear path and preserve InvalidParam.
    /// Interrupts have the same kernel-aware restrictions as [`Self::set_bits_from_isr`].
    #[inline]
    pub fn clear_bits_from_isr(&self, bits: u32) -> Result<()> {
        // SAFETY: pointer-free detection does not access the native object.
        if !unsafe { bindings::ove_is_in_isr() } {
            blocking_context()?;
            if bits & !Self::valid_mask() != 0 {
                return Err(Error::InvalidParam);
            }
            // SAFETY: admitted task, valid mask and borrowed live owner.
            unsafe { bindings::ove_eventgroup_clear_bits(self.handle, bits) };
            return Ok(());
        }
        Error::from_code(unsafe { bindings::ove_eventgroup_clear_bits_from_isr(self.handle, bits) })
    }

    /// Get current bits value.
    /// Returns inert zero without native effects when BLOCKING is unavailable;
    /// that fallback does not report the actual bits.
    #[inline]
    pub fn get_bits(&self) -> u32 {
        if blocking_context().is_err() {
            return 0;
        }
        unsafe { bindings::ove_eventgroup_get_bits(self.handle) }
    }

    /// Register a notify callback after each completed nonzero set.
    /// Wraps the C-level `ove_eventgroup_set_notify`.
    /// Setup requires RESOURCE permission.
    ///
    /// # Safety
    /// Serialize setup/replacement/removal with producers, deferred dispatch
    /// and other registrations. Keep callback code and `user_data` valid through
    /// pending dispatch and saved/in-flight tails, including after removal or
    /// native teardown; neither is a callback-data reclamation fence.
    /// Task sets notify inside their call. FreeRTOS ISR sets notify later in the
    /// timer daemon after native publication; other engines use the producer
    /// context. Rejected/canceled/zero sets and clears do not notify. Waiters
    /// may consume bits before notification. The callback must be nonblocking,
    /// safe in all delivery contexts, support permitted concurrent/reentrant
    /// calls and never unwind across C.
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        require_resource_context()?;
        let rc = unsafe { bindings::ove_eventgroup_set_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}

impl fmt::Debug for EventGroup {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("EventGroup")
            .field("handle", &self.handle)
            .finish_non_exhaustive()
    }
}

impl Drop for EventGroup {
    fn drop(&mut self) {
        if self.handle.is_null() || !resource_context() {
            return;
        }
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_eventgroup_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_eventgroup_deinit(self.handle);
        }
        self.origin.release_claim();
    }
}

// SAFETY: native bit/wait operations serialize access. The sole owner retains
// backing through admitted cleanup; synchronous borrows and native cancellation
// protect its lifetime. Notification registration/data have separate unsafe rules.
unsafe impl Send for EventGroup {}
// SAFETY: the same native serialization/lifecycle rules permit shared operations.
unsafe impl Sync for EventGroup {}
