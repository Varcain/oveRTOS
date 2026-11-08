// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Type-safe FIFO queue for oveRTOS.
//!
//! [`Queue<T, N>`] is a fixed-capacity, thread-safe FIFO that transfers items of
//! type `T` (which must be `Copy`) between threads or between ISR and thread
//! contexts. `N` is the maximum number of items the queue can hold.
//!
//! On NuttX, positive finite waits round up to ticks and saturate at
//! min(WDOG_MAX_DELAY - 2, INT32_MAX - 1) ticks: 1,073,741,821 with
//! 32-bit clocks or 2,147,483,646 with 64-bit clocks. Timeout and stream
//! short counts refer to this effective budget. Only explicit FOREVER
//! waits are indefinite; native tick phase, scheduling and condition-variable
//! mutex reacquisition can extend the elapsed time.

use core::cell::UnsafeCell;
use core::fmt;
use core::marker::PhantomData;
use core::mem;
use core::mem::MaybeUninit;
use core::sync::atomic::AtomicBool;
#[cfg(zero_heap)]
use core::sync::atomic::Ordering;

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY: successful native construction publishes an initialized nonnull
// handle. One owner retains its backing through complete admitted cleanup;
// operations borrow that owner through synchronous callback tails. Item pointers
// are aligned valid T values or uninitialized receive space for byte copies.
// Raw backing and notification lifetimes have additional unsafe entry contracts.

#[inline]
fn resource_context() -> bool {
    // SAFETY: pointer-free, allocation-free query with Queue-only availability.
    (unsafe { bindings::ove_sync_context() } & bindings::OVE_SYNC_CONTEXT_RESOURCE) != 0
}

#[inline]
fn require_resource_context() -> Result<()> {
    if resource_context() {
        Ok(())
    } else {
        Err(Error::NotSupported)
    }
}

#[inline]
fn blocking_context() -> Result<()> {
    // SAFETY: pointer-free query, before any clock or primitive operation.
    if (unsafe { bindings::ove_sync_context() } & bindings::OVE_SYNC_CONTEXT_BLOCKING) != 0 {
        Ok(())
    } else {
        Err(Error::NotSupported)
    }
}

#[derive(Clone, Copy)]
enum QueueOrigin {
    #[cfg(not(zero_heap))]
    Heap,
    #[cfg(zero_heap)]
    Static(&'static AtomicBool),
    #[cfg(zero_heap)]
    Raw,
}

impl QueueOrigin {
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

/// Caller-owned storage + item buffer for a [`Queue`] in zero-heap mode.
/// Declare in a `static` and pass `&STORAGE` to [`Queue::create`]; in heap
/// mode it is ignored. It embeds the native record and `[T; N]` item buffer.
/// Zero-heap initialization claims the backing until complete legal cleanup.
/// Live, forgotten and retained owners exclude a new claimant.
#[allow(dead_code)]
pub struct QueueStorage<T: Copy, const N: usize> {
    storage: UnsafeCell<MaybeUninit<bindings::ove_queue_storage_t>>,
    buffer: UnsafeCell<MaybeUninit<[T; N]>>,
    claimed: AtomicBool,
}

impl<T: Copy, const N: usize> QueueStorage<T, N> {
    /// Zero-initialised storage with an uninitialised item buffer.  `const`
    /// so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            buffer: UnsafeCell::new(MaybeUninit::uninit()),
            claimed: AtomicBool::new(false),
        }
    }
}

impl<T: Copy, const N: usize> Default for QueueStorage<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: private uninitialized bytes expose no payload references. A sole
// claimant owns native accesses; non-Send owners stay local. Complete cleanup
// precedes claim reopening, and initialization resets occupancy before reads.
// Forgotten/retained generations stay claimed, so no old payload is inherited.
unsafe impl<T: Copy, const N: usize> Sync for QueueStorage<T, N> {}

/// Type-safe FIFO queue with compile-time capacity.
///
/// `T` must be `Copy`: native byte transfers do not run a payload destructor.
/// `N` is the queue capacity. Owners are Send and Sync when T is Send.
/// Construction and destruction require native RESOURCE permission. Unsupported
/// Drop retains the native resource and its static claim; forget also retains it.
/// Every task transfer, including zero timeout, requires BLOCKING permission.
/// Unsupported task operations return [`Error::NotSupported`] before native or
/// deadline-clock effects. Borrowed heap payloads remain supported.
pub struct Queue<T: Copy, const N: usize> {
    handle: bindings::ove_queue_t,
    origin: QueueOrigin,
    _marker: PhantomData<UnsafeCell<T>>,
}

impl<T: Copy, const N: usize> Queue<T, N> {
    fn dimensions() -> Result<u32> {
        if N == 0 || mem::size_of::<T>() == 0 {
            return Err(Error::InvalidParam);
        }
        let capacity = u32::try_from(N).map_err(|_| Error::InvalidParam)?;
        // Validate multiplication and the addressable Rust array extent without
        // allocating. Native provider count ceilings remain checked by C.
        core::alloc::Layout::array::<T>(N).map_err(|_| Error::InvalidParam)?;
        Ok(capacity)
    }

    /// Create a queue via heap allocation (only in heap mode).
    /// Native initialization errors are propagated after partial resources are
    /// cleaned up. Native dimensions must fit the provider, including the
    /// NuttX semaphore count limit.
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        require_resource_context()?;
        let capacity = Self::dimensions()?;
        let mut handle: bindings::ove_queue_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_queue_create(&mut handle, mem::size_of::<T>(), capacity) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: QueueOrigin::Heap,
            _marker: PhantomData,
        })
    }

    /// Create from caller-provided static storage and buffer.
    ///
    /// # Safety
    /// The native record must be nonnull, aligned for `ove_queue_storage_t`,
    /// complete, writable, unused and disjoint from the complete writable item
    /// buffer of at least `N * size_of::<T>()` bytes. Current providers copy
    /// bytes and do not require T alignment for the buffer itself.
    /// Both extents must remain stable through actual native cleanup or
    /// indefinite retention after forget or unsupported Drop, including all
    /// pending callback tails. No aliased live Rust reference into the extents,
    /// competing initialization, teardown or raw/wrapper access is permitted.
    /// Native operation and interrupt-context contracts still apply.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_queue_storage_t,
        buffer: *mut core::ffi::c_void,
    ) -> Result<Self> {
        require_resource_context()?;
        let capacity = Self::dimensions()?;
        let mut handle: bindings::ove_queue_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_queue_init(&mut handle, storage, buffer, mem::size_of::<T>(), capacity)
        };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: QueueOrigin::Raw,
            _marker: PhantomData,
        })
    }

    /// Mode-agnostic constructor. Heap mode
    /// ignores `storage`; zero-heap mode backs the queue with its storage and
    /// embedded item buffer.
    /// In zero-heap mode, a live, initializing, tearing-down, forgotten or
    /// retained owner returns [`Error::Busy`] before native initialization.
    /// Clean initialization errors release the claim, preserving their error.
    pub fn create(storage: &'static QueueStorage<T, N>) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new()
        }
        #[cfg(zero_heap)]
        {
            require_resource_context()?;
            Self::dimensions()?;
            storage
                .claimed
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .map_err(|_| Error::Busy)?;
            let sptr = UnsafeCell::raw_get(&storage.storage).cast();
            let bptr = UnsafeCell::raw_get(&storage.buffer).cast::<core::ffi::c_void>();
            // SAFETY: a sole claim owns stable complete typed native/item backing.
            match unsafe { Self::from_static(sptr, bptr) } {
                Ok(mut owner) => {
                    owner.origin = QueueOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    // Native failure has completed all earlier-stage cleanup.
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }

    /// Send an item, blocking indefinitely if the queue is full.
    /// `std::sync::mpsc::SyncSender::send` analog.
    ///
    /// `item` is taken by `&T` rather than by value because the
    /// substrate `memcpy`s the bytes — `T: Copy` is enforced at the
    /// `Queue<T, N>` type level, so `&T` vs `T` is a wash semantically
    /// and `&T` avoids one stack-side `memcpy` for large `T`.
    #[inline]
    pub fn send(&self, item: &T) -> Result<()> {
        blocking_context()?;
        let rc = unsafe {
            bindings::ove_queue_send(self.handle, item as *const T as *const _, u64::MAX)
        };
        Error::from_code(rc)
    }

    /// Non-blocking send.  `std::sync::mpsc::SyncSender::try_send` analog.
    ///
    /// # Errors
    /// Returns [`Error::QueueFull`] if the queue is full.
    #[inline]
    pub fn try_send(&self, item: &T) -> Result<()> {
        blocking_context()?;
        let rc = unsafe { bindings::ove_queue_send(self.handle, item as *const T as *const _, 0) };
        Error::from_code(rc)
    }

    /// Send, waiting up to `d`.
    ///
    /// # Errors
    /// Returns [`Error::QueueFull`] / [`Error::Timeout`] if the queue
    /// stays full through `d`.
    #[inline]
    pub fn try_send_for(&self, item: &T, d: core::time::Duration) -> Result<()> {
        blocking_context()?;
        let rc = unsafe {
            bindings::ove_queue_send(
                self.handle,
                item as *const T as *const _,
                crate::duration::dur_to_ns(d),
            )
        };
        Error::from_code(rc)
    }

    /// Send by the given deadline.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    #[inline]
    #[cfg(has_time)]
    pub fn try_send_until(&self, item: &T, deadline: crate::time::Instant) -> Result<()> {
        blocking_context()?;
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc =
            unsafe { bindings::ove_queue_send(self.handle, item as *const T as *const _, timeout) };
        Error::from_code(rc)
    }

    /// Receive an item, blocking indefinitely.  `std::sync::mpsc::Receiver::recv`
    /// analog.
    #[inline]
    pub fn recv(&self) -> Result<T> {
        blocking_context()?;
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc = unsafe {
            bindings::ove_queue_receive(self.handle, item.as_mut_ptr() as *mut _, u64::MAX)
        };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }

    /// Non-blocking receive.  `std::sync::mpsc::Receiver::try_recv` analog.
    ///
    /// # Errors
    /// Returns [`Error::QueueEmpty`] if the queue is empty.
    #[inline]
    pub fn try_recv(&self) -> Result<T> {
        blocking_context()?;
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc =
            unsafe { bindings::ove_queue_receive(self.handle, item.as_mut_ptr() as *mut _, 0) };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }

    /// Receive, waiting up to `d`.
    ///
    /// # Errors
    /// Returns [`Error::QueueEmpty`] / [`Error::Timeout`] if no item is
    /// available within `d`.
    #[inline]
    pub fn try_recv_for(&self, d: core::time::Duration) -> Result<T> {
        blocking_context()?;
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc = unsafe {
            bindings::ove_queue_receive(
                self.handle,
                item.as_mut_ptr() as *mut _,
                crate::duration::dur_to_ns(d),
            )
        };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }

    /// Receive by the given deadline.
    #[inline]
    #[cfg(has_time)]
    pub fn try_recv_until(&self, deadline: crate::time::Instant) -> Result<T> {
        blocking_context()?;
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc = unsafe {
            bindings::ove_queue_receive(self.handle, item.as_mut_ptr() as *mut _, timeout)
        };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }

    /// Send an item from an ISR context (non-blocking, returns immediately if full).
    /// Actual task callers use the admitted task zero-timeout path. An interrupt
    /// must satisfy the native kernel-aware priority/setup contract; interrupt
    /// detection does not qualify NMI or arbitrary higher-priority handlers.
    ///
    /// # Errors
    /// Returns [`Error::QueueFull`] if the queue has no space.
    #[inline]
    pub fn send_from_isr(&self, item: &T) -> Result<()> {
        // SAFETY: pointer-free context detection requires no primitive access.
        if !unsafe { bindings::ove_is_in_isr() } {
            return self.try_send(item);
        }
        let rc =
            unsafe { bindings::ove_queue_send_from_isr(self.handle, item as *const T as *const _) };
        Error::from_code(rc)
    }

    /// Receive an item from an ISR context (non-blocking, returns immediately if empty).
    /// Task callers use the admitted task zero-timeout path. Interrupt callers
    /// have the same kernel-aware restrictions as [`Self::send_from_isr`].
    ///
    /// # Errors
    /// Returns [`Error::QueueEmpty`] if the queue is empty.
    #[inline]
    pub fn receive_from_isr(&self) -> Result<T> {
        // SAFETY: pointer-free context detection requires no primitive access.
        if !unsafe { bindings::ove_is_in_isr() } {
            return self.try_recv();
        }
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc = unsafe {
            bindings::ove_queue_receive_from_isr(self.handle, item.as_mut_ptr() as *mut _)
        };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }

    /// Number of items in the queue right now.  A snapshot: other threads
    /// can change it before you act on it.  Not for ISR context — use
    /// [`len_from_isr`](Self::len_from_isr) there.
    /// Returns inert zero without native access when BLOCKING is unavailable,
    /// including interrupts. This fallback does not report actual emptiness.
    #[inline]
    pub fn len(&self) -> usize {
        if blocking_context().is_err() {
            return 0;
        }
        unsafe { bindings::ove_queue_count(self.handle) }
    }

    /// Kernel-aware ISR count snapshot; task callers use [`Self::len`].
    /// Interrupt priority/setup restrictions match [`Self::send_from_isr`].
    /// Unsupported task contexts return inert zero.
    #[inline]
    pub fn len_from_isr(&self) -> usize {
        // SAFETY: pointer-free detection selects the native ISR contract.
        if !unsafe { bindings::ove_is_in_isr() } {
            return self.len();
        }
        unsafe { bindings::ove_queue_count_from_isr(self.handle) }
    }

    /// Maximum number of items the queue holds (`N`).
    #[inline]
    pub fn capacity(&self) -> usize {
        N
    }

    /// Whether the queue holds no item right now (snapshot, like [`len`](Self::len)).
    /// Unsupported task/interrupt contexts return true via the inert snapshot.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Whether the queue is full right now (snapshot, like [`len`](Self::len)).
    /// Unsupported task/interrupt contexts return false via the inert snapshot.
    #[inline]
    pub fn is_full(&self) -> bool {
        self.len() >= N
    }

    /// Register a notify callback fired after every successful send.
    /// Setup requires RESOURCE permission and can take a native mutex.
    ///
    /// # Safety
    /// Serialize registration, replacement and removal with every producer and
    /// registration. Callback code/data must survive all saved old callback
    /// tails; clear/replacement returning does not itself retire those tails.
    /// Support concurrent/reentrant delivery or externally serialize producers.
    /// Callback code must obey every producer's task/interrupt restrictions and
    /// must not unwind through C. Static lifetime alone does not serialize setup.
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        require_resource_context()?;
        let rc = unsafe { bindings::ove_queue_set_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}

impl<T: Copy, const N: usize> fmt::Debug for Queue<T, N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Queue")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}

impl<T: Copy, const N: usize> Drop for Queue<T, N> {
    fn drop(&mut self) {
        if self.handle.is_null() || !resource_context() {
            return;
        }
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_queue_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_queue_deinit(self.handle);
        }
        self.origin.release_claim();
    }
}

// SAFETY: `Queue<T, N>` wraps an `ove_queue_t` FIFO.  The substrate
// serialises producer/consumer access via internal locks; the `T: Copy +
// Send` bound ensures items can be safely transferred across threads.
unsafe impl<T: Copy + Send, const N: usize> Send for Queue<T, N> {}
unsafe impl<T: Copy + Send, const N: usize> Sync for Queue<T, N> {}
