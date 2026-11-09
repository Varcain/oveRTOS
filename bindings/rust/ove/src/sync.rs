// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

//! Synchronization primitives for oveRTOS.
//!
//! Mutex access runs in a callback scope. The binding privately releases the
//! native lock when the callback returns, even if its public view is forgotten.
//! Semaphores and events provide their ordinary wait/signal interfaces.
//! Absolute deadlines and timed predicate waits require `CONFIG_OVE_TIME`.
//!
//! On NuttX, positive finite waits round up to ticks and saturate at
//! min(WDOG_MAX_DELAY - 2, INT32_MAX - 1) ticks: 1,073,741,821 with
//! 32-bit clocks or 2,147,483,646 with 64-bit clocks. Timeout and stream
//! short counts refer to this effective budget. Only explicit FOREVER
//! waits are indefinite; native tick phase, scheduling and condition-variable
//! mutex reacquisition can extend the elapsed time.
//!
//! FreeRTOS synchronization, queue, EventGroup and stream calls convert a
//! positive finite encoded budget with exact ceil(ns * Hz / 1e9), saturating
//! at portMAX_DELAY - 1 before tick-type narrowing. Encoded FOREVER selects
//! portMAX_DELAY; indefinite native waiting requires INCLUDE_vTaskSuspend=1.
//! These are native-call budgets: scheduling, composed waits and mutex
//! reacquisition can extend elapsed time.

use core::cell::UnsafeCell;
use core::fmt;
use core::marker::PhantomData;
#[cfg(not(zero_heap))]
use core::mem::ManuallyDrop;
use core::mem::MaybeUninit;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::bindings;
use crate::error::{Error, Result};

fn resource_context() -> bool {
    // SAFETY: the query touches no primitive and is available with SYNC alone.
    unsafe { bindings::ove_sync_context() & bindings::OVE_SYNC_CONTEXT_RESOURCE != 0 }
}

fn producer_context() -> bool {
    // SAFETY: a pointer-free query; masked task producers do not block or allocate.
    unsafe { bindings::ove_sync_context() & bindings::OVE_SYNC_CONTEXT_PRODUCER != 0 }
}

fn blocking_context() -> Result<()> {
    // SAFETY: no primitive, allocator, clock or caller-owned pointer is involved.
    if unsafe { bindings::ove_sync_context() & bindings::OVE_SYNC_CONTEXT_BLOCKING != 0 } {
        Ok(())
    } else {
        Err(Error::NotSupported)
    }
}

fn require_resource_context() -> Result<()> {
    if resource_context() {
        Ok(())
    } else {
        Err(Error::NotSupported)
    }
}

#[derive(Clone, Copy)]
enum SyncOrigin {
    #[cfg(not(zero_heap))]
    Heap,
    #[cfg(zero_heap)]
    Static(&'static AtomicBool),
    #[cfg(zero_heap)]
    Raw,
}

impl SyncOrigin {
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

/// Stable backing for a [`Mutex`] in zero-heap mode.
/// A live or forgotten owner reserves it; another `create` returns Busy.
/// Heap mode ignores this storage. Legal Drop finishes deinit before reuse.
#[allow(dead_code)]
pub struct MutexStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_mutex_storage_t>>,
    claimed: AtomicBool,
}
impl MutexStorage {
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }
}
impl Default for MutexStorage {
    fn default() -> Self {
        Self::new()
    }
}
// SAFETY: only the successful atomic claimant can initialize/use this backing.
// Its claim survives forget and unavailable cleanup, and reopens after deinit.
unsafe impl Sync for MutexStorage {}

/// Native blocking mutex protecting `T`, accessed through callback scopes.
///
/// [`with_lock`](Self::with_lock) gives the callback a nonowning [`MutexAccess`].
/// Dropping or forgetting that view does not unlock early: end the callback to
/// release. Views and references borrowed from them cannot escape the callback.
/// Native blocking and priority inheritance are retained; there is no poisoning.
///
/// Acquisitions require an active non-idle thread with interrupts enabled.
/// Unsupported contexts return [`Error::NotSupported`] before native effects.
/// Creation also supports FreeRTOS before scheduler start. Drop retains native
/// backing when resource cleanup is unavailable, while dropping `T` normally.
/// Native cancellation/nonlocal exits are outside ordinary Rust scope cleanup.
///
/// ```no_run
/// # fn example() -> ove::Result<()> {
/// let mutex = ove::mutex!(0u32);
/// mutex.with_lock(|mut value| { *value += 1; Ok(()) })?;
/// # Ok(())
/// # }
/// ```
pub struct Mutex<T: ?Sized> {
    handle: bindings::ove_mutex_t,
    origin: SyncOrigin,
    issued: AtomicBool,
    data: UnsafeCell<T>,
}
// SAFETY: native exclusion plus one issued data view serializes access to T.
unsafe impl<T: ?Sized + Send> Send for Mutex<T> {}
unsafe impl<T: ?Sized + Send> Sync for Mutex<T> {}

impl<T> Mutex<T> {
    /// Allocate native backing in heap mode; no Rust allocator is required.
    #[cfg(not(zero_heap))]
    pub fn new(val: T) -> Result<Self> {
        require_resource_context()?;
        let mut handle = core::ptr::null_mut();
        // SAFETY: resource context is admitted; C initializes the output on success.
        Error::from_code(unsafe { bindings::ove_mutex_create(&mut handle) })?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Heap,
            issued: AtomicBool::new(false),
            data: UnsafeCell::new(val),
        })
    }

    /// Initialize exclusive, caller-owned native backing in zero-heap mode.
    ///
    /// # Safety
    /// `storage` must be nonnull, aligned, sufficiently large, unused and stable
    /// through actual native cleanup or indefinite retention after forget or an
    /// unsupported Drop context. No competing initialization, native access or
    /// teardown is permitted. All native operation/context contracts still apply.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_mutex_storage_t, val: T) -> Result<Self> {
        require_resource_context()?;
        let mut handle = core::ptr::null_mut();
        // SAFETY: caller supplies exclusive stable native backing.
        Error::from_code(unsafe { bindings::ove_mutex_init(&mut handle, storage) })?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Raw,
            issued: AtomicBool::new(false),
            data: UnsafeCell::new(val),
        })
    }

    /// Create with claimed static backing in zero-heap mode, or allocate in heap mode.
    /// A live/forgotten static owner makes another create return [`Error::Busy`].
    pub fn create(storage: &'static MutexStorage, val: T) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new(val)
        }
        #[cfg(zero_heap)]
        {
            require_resource_context()?;
            storage
                .claimed
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .map_err(|_| Error::Busy)?;
            // SAFETY: this claim is the sole initializer of stable typed backing.
            let result = unsafe { Self::from_static(storage.storage.get().cast(), val) };
            match result {
                Ok(mut owner) => {
                    owner.origin = SyncOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }

    /// Consume the owner and transfer `T` once, without locking.
    /// Native backing is retained if cleanup is unavailable. Zero-heap owners
    /// also have deinit on Drop; this extraction method is heap-only.
    #[cfg(not(zero_heap))]
    pub fn into_inner(self) -> Result<T> {
        let this = ManuallyDrop::new(self);
        if !this.issued.load(Ordering::Acquire) && resource_context() {
            // SAFETY: exclusive consumed owner, no outstanding scope, legal cleanup.
            unsafe { bindings::ove_mutex_destroy(this.handle) };
            this.origin.release_claim();
        }
        // SAFETY: consumed owner is not dropped; this transfers T exactly once.
        Ok(unsafe { core::ptr::read(this.data.get()) })
    }
}

impl<T: ?Sized> Mutex<T> {
    /// Borrow `T` exclusively without locking through an exclusive owner borrow.
    pub fn get_mut(&mut self) -> &mut T {
        // SAFETY: &mut self excludes every accessible data view/reference.
        unsafe { &mut *self.data.get() }
    }

    fn scoped<R>(
        &self,
        timeout: u64,
        f: impl for<'s> FnOnce(MutexAccess<'s, T>) -> Result<R>,
    ) -> Result<R> {
        blocking_context()?;
        // SAFETY: live owned native handle, admitted blocking context.
        Error::from_code(unsafe { bindings::ove_mutex_lock(self.handle, timeout) })?;
        if self.issued.swap(true, Ordering::AcqRel) {
            // SAFETY: native recursion acquired only this temporary level; the
            // previous view/lease remains intact and no second view was issued.
            unsafe { bindings::ove_mutex_unlock(self.handle) };
            return Err(Error::Busy);
        }
        let mut lease = MutexLease {
            mutex: self,
            held: true,
        };
        f(MutexAccess {
            mutex: self,
            held: &mut lease.held,
            invariant: PhantomData,
            no_send: PhantomData,
        })
    }

    /// Run `f` with exclusive data access, waiting indefinitely for the lock.
    pub fn with_lock<R>(
        &self,
        f: impl for<'s> FnOnce(MutexAccess<'s, T>) -> Result<R>,
    ) -> Result<R> {
        self.scoped(u64::MAX, f)
    }
    /// Attempt the same scope without waiting; native contention returns an error.
    pub fn try_with_lock<R>(
        &self,
        f: impl for<'s> FnOnce(MutexAccess<'s, T>) -> Result<R>,
    ) -> Result<R> {
        self.scoped(0, f)
    }
    /// Wait up to `duration` to enter the callback scope.
    pub fn try_with_lock_for<R>(
        &self,
        duration: core::time::Duration,
        f: impl for<'s> FnOnce(MutexAccess<'s, T>) -> Result<R>,
    ) -> Result<R> {
        self.scoped(crate::duration::dur_to_ns(duration), f)
    }
    /// Wait until `deadline` to enter the scope. FOREVER means indefinite waiting.
    #[cfg(has_time)]
    pub fn try_with_lock_until<R>(
        &self,
        deadline: crate::time::Instant,
        f: impl for<'s> FnOnce(MutexAccess<'s, T>) -> Result<R>,
    ) -> Result<R> {
        blocking_context()?;
        self.scoped(crate::time::deadline_to_timeout_ns(deadline), f)
    }
}
impl<T: ?Sized> Drop for Mutex<T> {
    fn drop(&mut self) {
        if self.issued.load(Ordering::Acquire) || !resource_context() {
            return;
        }
        // SAFETY: exclusive owner, no outstanding acquisition, admitted cleanup.
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_mutex_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_mutex_deinit(self.handle);
        }
        self.origin.release_claim();
        // UnsafeCell<T> still drops T once, including on retained-backing paths.
    }
}
impl<T: ?Sized + fmt::Debug> fmt::Debug for Mutex<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Mutex")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}

struct MutexLease<'a, T: ?Sized> {
    mutex: &'a Mutex<T>,
    held: bool,
}
impl<T: ?Sized> Drop for MutexLease<'_, T> {
    fn drop(&mut self) {
        // An ordinary synchronous callback stays on its acquiring thread.
        // IRQ masks may change; they do not prevent this owned native release.
        if self.held && !unsafe { bindings::ove_is_in_isr() } {
            self.mutex.issued.store(false, Ordering::Release);
            // SAFETY: checked wait tracks whether this same thread still owns
            // exactly this level; no public view/reference can escape the scope.
            unsafe { bindings::ove_mutex_unlock(self.mutex.handle) };
        }
    }
}

/// Nonowning data view valid only inside one [`Mutex::with_lock`] callback.
/// It is !Send, and Sync when T:Sync. Drop/forget does not release the native
/// mutex; end the callback to release. CV waits consume and return this view.
/// Errors consume it while preserving the binding's private cleanup obligation.
pub struct MutexAccess<'s, T: ?Sized> {
    mutex: &'s Mutex<T>,
    held: &'s mut bool,
    invariant: PhantomData<fn(&'s ()) -> &'s ()>,
    no_send: PhantomData<*mut ()>,
}
// SAFETY: shared access exposes only &T, and cannot move/consume the !Send view.
unsafe impl<T: ?Sized + Sync> Sync for MutexAccess<'_, T> {}
impl<T: ?Sized> Deref for MutexAccess<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: this is the sole issued view and the private lease is held.
        unsafe { &*self.mutex.data.get() }
    }
}
impl<T: ?Sized> DerefMut for MutexAccess<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: native exclusion, a sole issued view and its exclusive borrow.
        unsafe { &mut *self.mutex.data.get() }
    }
}
impl<T: ?Sized + fmt::Debug> fmt::Debug for MutexAccess<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MutexAccess")
            .field("data", &&**self)
            .finish()
    }
}
impl<T: ?Sized + fmt::Display> fmt::Display for MutexAccess<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        (**self).fmt(f)
    }
}

/// Claimed native backing for a [`RecursiveMutex`] in zero-heap mode.
#[allow(dead_code)]
pub struct RecursiveMutexStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_mutex_storage_t>>,
    claimed: AtomicBool,
}
impl RecursiveMutexStorage {
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }
}
impl Default for RecursiveMutexStorage {
    fn default() -> Self {
        Self::new()
    }
}
// SAFETY: one atomic claim owns native backing through completed deinit.
unsafe impl Sync for RecursiveMutexStorage {}

/// Recursive native mutex with private, callback-scoped release ownership.
/// Each callback releases its level on return/Err or supported unwind. At most
/// 65,535 combined held and pending acquisitions are admitted; excess is Busy.
/// Context, retention and cancellation rules match [`Mutex`].
pub struct RecursiveMutex {
    handle: bindings::ove_mutex_t,
    origin: SyncOrigin,
    reservations: AtomicU32,
}
// SAFETY: owned stable native backing; scopes remain on their acquiring thread.
unsafe impl Send for RecursiveMutex {}
unsafe impl Sync for RecursiveMutex {}
impl RecursiveMutex {
    const MAX_RESERVATIONS: u32 = 65_535;
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        require_resource_context()?;
        let mut handle = core::ptr::null_mut();
        // SAFETY: admitted context; success publishes an initialized handle.
        Error::from_code(unsafe { bindings::ove_recursive_mutex_create(&mut handle) })?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Heap,
            reservations: AtomicU32::new(0),
        })
    }
    /// Initialize exclusive stable raw backing.
    ///
    /// # Safety
    /// Requirements match [`Mutex::from_static`]: complete aligned unused backing,
    /// exclusive native access, and lifetime through cleanup or retention.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_mutex_storage_t) -> Result<Self> {
        require_resource_context()?;
        let mut handle = core::ptr::null_mut();
        // SAFETY: caller supplies exclusive stable native backing.
        Error::from_code(unsafe { bindings::ove_recursive_mutex_init(&mut handle, storage) })?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Raw,
            reservations: AtomicU32::new(0),
        })
    }
    pub fn create(storage: &'static RecursiveMutexStorage) -> Result<Self> {
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
            // SAFETY: the successful claim exclusively owns stable native backing.
            match unsafe { Self::from_static(storage.storage.get().cast()) } {
                Ok(mut owner) => {
                    owner.origin = SyncOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }
    fn scoped<R>(&self, timeout: u64, f: impl FnOnce() -> Result<R>) -> Result<R> {
        blocking_context()?;
        self.reservations
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| {
                (n < Self::MAX_RESERVATIONS).then(|| n + 1)
            })
            .map_err(|_| Error::Busy)?;
        // SAFETY: live native handle, legal blocking context and bounded reservation.
        if let Err(error) =
            Error::from_code(unsafe { bindings::ove_recursive_mutex_lock(self.handle, timeout) })
        {
            self.reservations.fetch_sub(1, Ordering::Release);
            return Err(error);
        }
        let _lease = RecursiveLease { mutex: self };
        f()
    }
    pub fn with_lock<R>(&self, f: impl FnOnce() -> Result<R>) -> Result<R> {
        self.scoped(u64::MAX, f)
    }
    pub fn try_with_lock<R>(&self, f: impl FnOnce() -> Result<R>) -> Result<R> {
        self.scoped(0, f)
    }
    pub fn try_with_lock_for<R>(
        &self,
        duration: core::time::Duration,
        f: impl FnOnce() -> Result<R>,
    ) -> Result<R> {
        self.scoped(crate::duration::dur_to_ns(duration), f)
    }
    #[cfg(has_time)]
    pub fn try_with_lock_until<R>(
        &self,
        deadline: crate::time::Instant,
        f: impl FnOnce() -> Result<R>,
    ) -> Result<R> {
        blocking_context()?;
        self.scoped(crate::time::deadline_to_timeout_ns(deadline), f)
    }
}
impl Drop for RecursiveMutex {
    fn drop(&mut self) {
        if self.reservations.load(Ordering::Acquire) != 0 || !resource_context() {
            return;
        }
        // SAFETY: no pending/held scope, exclusive owner and legal resource cleanup.
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_recursive_mutex_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_recursive_mutex_deinit(self.handle);
        }
        self.origin.release_claim();
    }
}
impl fmt::Debug for RecursiveMutex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("RecursiveMutex")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}
struct RecursiveLease<'a> {
    mutex: &'a RecursiveMutex,
}

#[cfg(all(test, ove_test))]
#[path = "sync_tests.rs"]
mod scoped_tests;
impl Drop for RecursiveLease<'_> {
    fn drop(&mut self) {
        if !unsafe { bindings::ove_is_in_isr() } {
            // SAFETY: exactly this private acquisition belongs to this thread.
            unsafe { bindings::ove_recursive_mutex_unlock(self.mutex.handle) };
            self.mutex.reservations.fetch_sub(1, Ordering::Release);
        }
    }
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/// Claimed native backing for a [`Semaphore`] in zero-heap mode.
/// A live, forgotten or retained owner reserves it until completed cleanup.
/// Heap construction ignores this storage.
#[allow(dead_code)]
pub struct SemaphoreStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_sem_storage_t>>,
    claimed: AtomicBool,
}

impl SemaphoreStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }
}

impl Default for SemaphoreStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: only the successful atomic claimant initializes or cleans this backing.
unsafe impl Sync for SemaphoreStorage {}

/// Counting semaphore.
///
/// Counts require `initial <= max`, `max > 0`, and the native provider ceiling:
/// `SEM_VALUE_MAX` on POSIX/NuttX, `UBaseType_t` on FreeRTOS, and `unsigned int`
/// on Zephyr. POSIX/NuttX posts enforce the provider ceiling rather than the
/// requested `max`; FreeRTOS/Zephyr retain and enforce that requested maximum.
///
/// Creation and cleanup follow [`Mutex`]'s resource-context and retention rules.
/// Every take requires an active non-idle thread with interrupts enabled;
/// unsupported contexts return [`Error::NotSupported`] before native/clock effects.
/// Task releases and serialized notification setup also support recognized
/// masked contexts and FreeRTOS before scheduler start or while suspended.
/// ISR/unsupported releases have no effect: no permit and no callback.
pub struct Semaphore {
    handle: bindings::ove_sem_t,
    origin: SyncOrigin,
}

// SAFETY: owned stable backing; native counter/waiter operations are thread-safe.
unsafe impl Send for Semaphore {}
unsafe impl Sync for Semaphore {}

impl Semaphore {
    /// Create a counting semaphore via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new(initial: u32, max: u32) -> Result<Self> {
        require_resource_context()?;
        let mut handle: bindings::ove_sem_t = core::ptr::null_mut();
        // SAFETY: admitted resource context; success publishes initialized backing.
        let rc = unsafe { bindings::ove_sem_create(&mut handle, initial, max) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Heap,
        })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Backing must be nonnull, aligned, complete, writable, unused and stable
    /// through actual native cleanup or indefinite retention after forget or
    /// unsupported Drop. No competing initialization, teardown or raw/wrapper
    /// access is permitted. Native operation/context contracts still apply.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_sem_storage_t,
        initial: u32,
        max: u32,
    ) -> Result<Self> {
        require_resource_context()?;
        let mut handle: bindings::ove_sem_t = core::ptr::null_mut();
        // SAFETY: caller supplies exclusive stable native backing.
        let rc = unsafe { bindings::ove_sem_init(&mut handle, storage, initial, max) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Raw,
        })
    }

    /// Create a counting semaphore that works in **both** heap and zero-heap
    /// modes. Heap mode ignores `storage`; zero-heap mode claims its backing.
    /// A live, forgotten or retained owner returns [`Error::Busy`] before native
    /// initialization. Legal Drop completes cleanup before reopening the claim.
    pub fn create(storage: &'static SemaphoreStorage, initial: u32, max: u32) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new(initial, max)
        }
        #[cfg(zero_heap)]
        {
            require_resource_context()?;
            storage
                .claimed
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .map_err(|_| Error::Busy)?;
            // SAFETY: this claim exclusively owns stable typed native backing.
            match unsafe { Self::from_static(storage.storage.get().cast(), initial, max) } {
                Ok(mut owner) => {
                    owner.origin = SyncOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }

    /// Acquire one permit, blocking indefinitely.  `tokio::sync::Semaphore::acquire`
    /// analog (sans `.await`).
    #[inline]
    pub fn acquire(&self) -> Result<()> {
        blocking_context()?;
        // SAFETY: borrowed live owner and admitted blocking context.
        let rc = unsafe { bindings::ove_sem_take(self.handle, u64::MAX) };
        Error::from_code(rc)
    }

    /// Attempt to acquire one permit without blocking.
    ///
    /// # Errors
    /// Preserves the native empty/timeout error if no permit is available.
    /// Unsupported context returns [`Error::NotSupported`].
    #[inline]
    pub fn try_acquire(&self) -> Result<()> {
        blocking_context()?;
        // SAFETY: live owner and admitted task context, including zero timeout.
        let rc = unsafe { bindings::ove_sem_take(self.handle, 0) };
        Error::from_code(rc)
    }

    /// Attempt to acquire one permit, waiting up to `d`.
    /// `parking_lot::Semaphore` doesn't exist; this matches the
    /// `try_lock_for` convention.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the duration elapses with no
    /// permit available.
    #[inline]
    pub fn try_acquire_for(&self, d: core::time::Duration) -> Result<()> {
        blocking_context()?;
        // SAFETY: live owner and admitted blocking context.
        let rc = unsafe { bindings::ove_sem_take(self.handle, crate::duration::dur_to_ns(d)) };
        Error::from_code(rc)
    }

    /// Attempt to acquire one permit by the given deadline.
    /// Use [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    #[inline]
    #[cfg(has_time)]
    pub fn try_acquire_until(&self, deadline: crate::time::Instant) -> Result<()> {
        blocking_context()?;
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        // SAFETY: context admission precedes both clock and native wait.
        let rc = unsafe { bindings::ove_sem_take(self.handle, timeout) };
        Error::from_code(rc)
    }

    /// Release one permit.  `tokio::sync::Semaphore::add_permits(1)` /
    /// `embassy_sync::Semaphore::release(1)` analog.
    /// No effect in ISR or unsupported task context, including no wake callback.
    #[inline]
    pub fn release(&self) {
        if producer_context() {
            // SAFETY: live owner and admitted task producer, which may be masked.
            unsafe { bindings::ove_sem_give(self.handle) }
        }
    }

    /// Release `n` permits.  Binding-side loop — substrate currently
    /// has no `ove_sem_give_n`, so this calls `ove_sem_give` `n` times.
    /// ISR/unsupported entry and zero permits perform no native post or callback.
    #[inline]
    pub fn release_n(&self, n: u32) {
        if !producer_context() {
            return;
        }
        for _ in 0..n {
            // SAFETY: live borrowed owner and admitted task producer.
            unsafe { bindings::ove_sem_give(self.handle) }
        }
    }

    /// Register a callback invoked by the native give path after a post attempt.
    /// Current providers also invoke it when their count ceiling prevents an
    /// increment. Setup requires admitted task producer context.
    ///
    /// # Safety
    /// Serialize registration, replacement and removal with every native give
    /// and every other registration. Callback code and `user_data` must stay
    /// valid through all possible in-flight calls; retire the previous callback
    /// only after those calls finish. The callback must obey every give caller's
    /// execution context and native restrictions and must not unwind through C.
    /// Callback code and data must also support concurrent or reentrant calls
    /// from all release callers, or the caller must serialize those gives.
    /// Static lifetime alone does not provide registration synchronization.
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        if !producer_context() {
            return Err(Error::NotSupported);
        }
        // SAFETY: caller provides serialized callback lifetime; task setup admitted.
        let rc = unsafe { bindings::ove_sem_set_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}

impl Drop for Semaphore {
    fn drop(&mut self) {
        if !resource_context() {
            return;
        }
        // SAFETY: exclusive owner, no borrowed operation, admitted resource cleanup.
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_sem_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_sem_deinit(self.handle);
        }
        self.origin.release_claim();
    }
}

impl fmt::Debug for Semaphore {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Semaphore")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

/// Claimed native backing for an [`Event`] in zero-heap mode.
/// Live, forgotten and retained owners reserve it through completed cleanup.
/// Heap construction ignores this storage.
#[allow(dead_code)]
pub struct EventStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_event_storage_t>>,
    claimed: AtomicBool,
}

impl EventStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }
}

impl Default for EventStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: only the successful atomic claimant initializes or cleans this backing.
unsafe impl Sync for EventStorage {}

/// Binary event (signal/wait).
///
/// Shared references support concurrent waiters. One pending occurrence permits
/// at most one successful wait, without a FIFO guarantee. Signals coalesce while
/// an occurrence remains pending; polls and timeouts preserve other waiters.
///
/// Creation, waits and cleanup use the same resource/blocking admission and
/// retention policy as [`Semaphore`]. Both producer methods select the actual
/// task/ISR native route, independent of their name. Recognized masked task
/// producers remain supported so critical-section and async wakes are delivered.
/// Unsupported task producer contexts have no effect.
/// ISR signaling supports only the provider's kernel-aware interrupt domain;
/// native interrupt setup must obey its syscall priority/grouping restrictions.
/// This does not qualify arbitrary high-priority interrupts or NMI.
pub struct Event {
    handle: bindings::ove_event_t,
    origin: SyncOrigin,
}

// SAFETY: owned stable backing; native event/waiter operations are thread-safe.
unsafe impl Send for Event {}
unsafe impl Sync for Event {}

impl Event {
    /// Create a new event via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        require_resource_context()?;
        let mut handle: bindings::ove_event_t = core::ptr::null_mut();
        // SAFETY: admitted resource context; success initializes native backing.
        let rc = unsafe { bindings::ove_event_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Heap,
        })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Backing must be nonnull, aligned, complete, writable, unused and stable
    /// through actual native cleanup or indefinite retention after forget or
    /// unsupported Drop. No conflicting initialization, teardown or raw/wrapper
    /// access; respect native operation contexts and waiter lifetimes.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_event_storage_t) -> Result<Self> {
        require_resource_context()?;
        let mut handle: bindings::ove_event_t = core::ptr::null_mut();
        // SAFETY: caller supplies exclusive stable native backing.
        let rc = unsafe { bindings::ove_event_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Raw,
        })
    }

    /// Claim typed static backing in zero-heap mode, or allocate in heap mode.
    /// Live, forgotten or retained static owners return [`Error::Busy`] before
    /// native entry. A legal Drop completes cleanup before the claim reopens.
    pub fn create(storage: &'static EventStorage) -> Result<Self> {
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
            // SAFETY: this claim exclusively owns stable typed native backing.
            match unsafe { Self::from_static(storage.storage.get().cast()) } {
                Ok(mut owner) => {
                    owner.origin = SyncOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }

    /// Block indefinitely until the event is signalled.
    #[inline]
    pub fn wait(&self) -> Result<()> {
        blocking_context()?;
        // SAFETY: borrowed live owner and admitted blocking context.
        let rc = unsafe { bindings::ove_event_wait(self.handle, u64::MAX) };
        Error::from_code(rc)
    }

    /// Non-blocking check.
    ///
    /// # Errors
    /// Preserves the native empty/timeout error if the event is not signalled.
    /// Unsupported context returns [`Error::NotSupported`].
    #[inline]
    pub fn try_wait(&self) -> Result<()> {
        blocking_context()?;
        // SAFETY: live owner and admitted task context, including zero timeout.
        let rc = unsafe { bindings::ove_event_wait(self.handle, 0) };
        Error::from_code(rc)
    }

    /// Wait for the event up to `d`.  `parking_lot::Condvar::wait_for`
    /// naming convention (waiting primitives don't use the `try_`
    /// prefix in parking_lot).
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the event is not signalled within
    /// `d`.
    #[inline]
    pub fn wait_for(&self, d: core::time::Duration) -> Result<()> {
        blocking_context()?;
        // SAFETY: live owner and admitted blocking context.
        let rc = unsafe { bindings::ove_event_wait(self.handle, crate::duration::dur_to_ns(d)) };
        Error::from_code(rc)
    }

    /// Wait for the event by the given deadline.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the deadline elapses before the event
    /// is signalled.
    #[inline]
    #[cfg(has_time)]
    pub fn wait_until(&self, deadline: crate::time::Instant) -> Result<()> {
        blocking_context()?;
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        // SAFETY: admission precedes both clock and native wait.
        let rc = unsafe { bindings::ove_event_wait(self.handle, timeout) };
        Error::from_code(rc)
    }

    /// Signal through the actual task or native kernel-aware ISR route.
    /// Masked task producers are supported; unsupported task contexts do nothing.
    #[inline]
    pub fn signal(&self) {
        // SAFETY: pointer-free current-context observation, available with SYNC.
        if unsafe { bindings::ove_is_in_isr() } {
            // SAFETY: live event; native setup supplies a kernel-aware ISR context.
            unsafe { bindings::ove_event_signal_from_isr(self.handle) }
        } else if producer_context() {
            // SAFETY: admitted live task producer, including native critical sections.
            unsafe { bindings::ove_event_signal(self.handle) }
        }
    }

    /// Signal using the actual context, including task callers of this method.
    /// ISR priority/setup restrictions and task no-effect rules match [`Self::signal`].
    #[inline]
    pub fn signal_from_isr(&self) {
        self.signal();
    }
}

impl Drop for Event {
    fn drop(&mut self) {
        if !resource_context() {
            return;
        }
        // SAFETY: exclusive owner, no borrowed operation, admitted resource cleanup.
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_event_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_event_deinit(self.handle);
        }
        self.origin.release_claim();
    }
}

impl fmt::Debug for Event {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Event")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}

// ---------------------------------------------------------------------------
// CondVar + WaitTimeoutResult
// ---------------------------------------------------------------------------

/// Outcome of a timed condition-variable wait with reacquired data access.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WaitTimeoutResult {
    timed_out: bool,
}
impl WaitTimeoutResult {
    pub fn timed_out(&self) -> bool {
        self.timed_out
    }
}

/// Claimed native backing for a [`CondVar`] in zero-heap mode.
#[allow(dead_code)]
pub struct CondVarStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_condvar_storage_t>>,
    claimed: AtomicBool,
}
impl CondVarStorage {
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            claimed: AtomicBool::new(false),
        }
    }
}
impl Default for CondVarStorage {
    fn default() -> Self {
        Self::new()
    }
}
// SAFETY: one atomic claimant controls initialization and completed deinit.
unsafe impl Sync for CondVarStorage {}

/// Condition variable used inside a [`Mutex::with_lock`] callback.
/// Wait consumes the current [`MutexAccess`] and returns it only after known
/// successful reacquisition or clean timeout. An error consumes the view; the
/// private scope still releases any known-held native acquisition on return.
/// Recheck predicates after wakeups. Timed predicate waits keep one deadline.
///
/// Context and resource retention rules match [`Mutex`]. Signal/broadcast are
/// no-ops outside an admitted blocking context; native NuttX signaling can lock.
/// Single-core FreeRTOS protects notification and waiter retirement together;
/// broadcast interrupt-masked work is linear in waiters. FreeRTOS SMP is unsupported.
/// Native V1 list operations use short privileged scopes and restore caller
/// mode before release, waiting and reacquisition. Privileged producers retain
/// incoming masks with brief fault/IRQ masking if needed; the blocking-context
/// admission above remains required for this wrapper.
pub struct CondVar {
    handle: bindings::ove_condvar_t,
    origin: SyncOrigin,
}
// SAFETY: stable owned native backing; operation borrows exclude teardown.
unsafe impl Send for CondVar {}
unsafe impl Sync for CondVar {}
impl CondVar {
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        require_resource_context()?;
        let mut handle = core::ptr::null_mut();
        // SAFETY: legal resource context, initialized output only on success.
        Error::from_code(unsafe { bindings::ove_condvar_create(&mut handle) })?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Heap,
        })
    }
    /// Initialize exclusive stable raw backing.
    ///
    /// # Safety
    /// Backing must be nonnull, aligned, sufficiently large, unused and stable
    /// through actual deinit or indefinite retention. No competing native access,
    /// initialization or teardown; respect native contexts and waiter lifetimes.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_condvar_storage_t) -> Result<Self> {
        require_resource_context()?;
        let mut handle = core::ptr::null_mut();
        // SAFETY: caller supplies exclusive complete stable native backing.
        Error::from_code(unsafe { bindings::ove_condvar_init(&mut handle, storage) })?;
        Ok(Self {
            handle,
            origin: SyncOrigin::Raw,
        })
    }
    pub fn create(storage: &'static CondVarStorage) -> Result<Self> {
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
            // SAFETY: one successful claim owns stable native backing.
            match unsafe { Self::from_static(storage.storage.get().cast()) } {
                Ok(mut owner) => {
                    owner.origin = SyncOrigin::Static(&storage.claimed);
                    Ok(owner)
                }
                Err(error) => {
                    storage.claimed.store(false, Ordering::Release);
                    Err(error)
                }
            }
        }
    }
    fn wait_with_timeout<'s, T: ?Sized>(
        &self,
        access: MutexAccess<'s, T>,
        timeout: u64,
    ) -> Result<(MutexAccess<'s, T>, WaitTimeoutResult)> {
        blocking_context()?;
        access.mutex.issued.store(false, Ordering::Release);
        let mut held = false;
        // SAFETY: live CV, ordinary mutex held exactly once, no outstanding data
        // reference after consuming access, and writable disjoint local output.
        let rc = unsafe {
            bindings::ove_condvar_wait_checked(self.handle, access.mutex.handle, timeout, &mut held)
        };
        *access.held = held;
        if held {
            // Native ownership excludes any other issuing thread. On unheld
            // errors another thread may own the flag, so leave it untouched.
            access.mutex.issued.store(true, Ordering::Release);
        }
        let timed_out = match Error::from_code(rc) {
            Ok(()) => false,
            Err(Error::Timeout) => true,
            Err(error) => return Err(error),
        };
        if !held {
            return Err(Error::Inval);
        }
        Ok((access, WaitTimeoutResult { timed_out }))
    }
    /// Release the access mutex while waiting indefinitely, then return access.
    /// Spurious wakes are permitted; prefer [`wait_while`](Self::wait_while).
    pub fn wait<'s, T: ?Sized>(&self, access: MutexAccess<'s, T>) -> Result<MutexAccess<'s, T>> {
        let (access, result) = self.wait_with_timeout(access, u64::MAX)?;
        if result.timed_out() {
            Err(Error::Timeout)
        } else {
            Ok(access)
        }
    }
    /// Wait at most `duration`; clean timeout returns reacquired access and true.
    pub fn wait_for<'s, T: ?Sized>(
        &self,
        access: MutexAccess<'s, T>,
        duration: core::time::Duration,
    ) -> Result<(MutexAccess<'s, T>, WaitTimeoutResult)> {
        self.wait_with_timeout(access, crate::duration::dur_to_ns(duration))
    }
    #[cfg(has_time)]
    pub fn wait_until<'s, T: ?Sized>(
        &self,
        access: MutexAccess<'s, T>,
        deadline: crate::time::Instant,
    ) -> Result<(MutexAccess<'s, T>, WaitTimeoutResult)> {
        blocking_context()?;
        self.wait_with_timeout(access, crate::time::deadline_to_timeout_ns(deadline))
    }
    /// Keep waiting while `predicate` is true, consuming access at every wait.
    pub fn wait_while<'s, T: ?Sized>(
        &self,
        mut access: MutexAccess<'s, T>,
        mut predicate: impl FnMut(&mut T) -> bool,
    ) -> Result<MutexAccess<'s, T>> {
        blocking_context()?;
        while predicate(&mut *access) {
            access = self.wait(access)?;
        }
        Ok(access)
    }
    #[cfg(has_time)]
    pub fn wait_while_for<'s, T: ?Sized>(
        &self,
        access: MutexAccess<'s, T>,
        duration: core::time::Duration,
        predicate: impl FnMut(&mut T) -> bool,
    ) -> Result<(MutexAccess<'s, T>, WaitTimeoutResult)> {
        blocking_context()?;
        let deadline = crate::time::Instant::now() + duration;
        self.wait_while_until(access, deadline, predicate)
    }
    #[cfg(has_time)]
    pub fn wait_while_until<'s, T: ?Sized>(
        &self,
        mut access: MutexAccess<'s, T>,
        deadline: crate::time::Instant,
        mut predicate: impl FnMut(&mut T) -> bool,
    ) -> Result<(MutexAccess<'s, T>, WaitTimeoutResult)> {
        blocking_context()?;
        loop {
            if !predicate(&mut *access) {
                return Ok((access, WaitTimeoutResult { timed_out: false }));
            }
            let (next, result) = self.wait_until(access, deadline)?;
            access = next;
            if result.timed_out() {
                let timed_out = predicate(&mut *access);
                return Ok((access, WaitTimeoutResult { timed_out }));
            }
        }
    }
    /// Wake one waiter. No effect when blocking context is unavailable.
    pub fn signal(&self) {
        if blocking_context().is_ok() {
            // SAFETY: live borrowed native CV and admitted signaling context.
            unsafe { bindings::ove_condvar_signal(self.handle) };
        }
    }
    /// Wake all waiters. No effect when blocking context is unavailable.
    pub fn broadcast(&self) {
        if blocking_context().is_ok() {
            // SAFETY: live borrowed native CV and admitted signaling context.
            unsafe { bindings::ove_condvar_broadcast(self.handle) };
        }
    }
}
impl Drop for CondVar {
    fn drop(&mut self) {
        if !resource_context() {
            return;
        }
        // SAFETY: exclusive owned CV, no borrowed operation/waiter, legal cleanup.
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_condvar_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_condvar_deinit(self.handle);
        }
        self.origin.release_claim();
    }
}
impl fmt::Debug for CondVar {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CondVar")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}

// ── SpinMutex (G5) ───────────────────────────────────────────────────
//
// IRQ-locking mutex for very short critical sections. Distinct from
// the sleeping `Mutex<T>` above:
//
// - `Mutex::with_lock` blocks the calling thread on a kernel mutex; legal
//   from thread context only.
// - `SpinMutex::with` holds native IRQ exclusion and an exclusive data
//   claim for a short callback. Use it for shared updates between polls
//   or await points, subject to the native provider's context contract.
//
// Requires the async substrate (`CONFIG_OVE_ASYNC=y`) — `ove_irq_lock`
// lives in `include/ove/irq.h` under that gate. Consumers that need to
// be portable across builds with `CONFIG_OVE_ASYNC=n` should gate with
// `#[cfg(has_async)]`.

#[cfg(has_async)]
pub use spin_mutex::SpinMutex;

#[cfg(has_async)]
mod spin_mutex {
    use ::core::cell::UnsafeCell;
    use ::core::marker::PhantomData;
    use ::core::sync::atomic::{AtomicBool, Ordering};

    use crate::bindings;

    /// IRQ-locking mutex for very short critical sections.
    ///
    /// Access through [`with`](Self::with), which holds a native IRQ key
    /// and one exclusive data claim until its callback returns or unwinds.
    /// Reentering the same mutex panics after restoring the failed attempt's
    /// temporary key. Callbacks must be short and must not block or await.
    ///
    /// Native IRQ-provider context and priority restrictions still apply.
    /// On FreeRTOS, only kernel-aware IRQs participate; unsupported entry
    /// stops before the callback. Restricted MPU V1 calls require a serviceable
    /// priority-zero SVCall with PRIMASK and FAULTMASK clear. Native calls in
    /// the callback can lower stronger incoming raw masks even though matching
    /// wrapper boundaries restore the captured state. This is not global IRQ
    /// exclusion or continuous stronger-mask protection through native calls.
    /// This type requires native SYNC and ASYNC configuration. A terminal
    /// panic or abort does not perform ordinary unwind cleanup.
    pub struct SpinMutex<T: ?Sized> {
        claimed: AtomicBool,
        inner: UnsafeCell<T>,
    }

    // SAFETY: the atomic claim admits one mutable view; callback lifetimes
    // prevent escape, and private leases retain native key order/cleanup.
    unsafe impl<T: ?Sized + Send> Send for SpinMutex<T> {}
    unsafe impl<T: ?Sized + Send> Sync for SpinMutex<T> {}

    impl<T> SpinMutex<T> {
        /// Wrap `value` in a new `SpinMutex`.
        #[inline]
        pub const fn new(value: T) -> Self {
            Self {
                claimed: AtomicBool::new(false),
                inner: UnsafeCell::new(value),
            }
        }

        /// Consume the mutex and return the wrapped value.
        #[inline]
        pub fn into_inner(self) -> T {
            self.inner.into_inner()
        }
    }

    impl<T: ?Sized> SpinMutex<T> {
        /// Run `f` with the value, restoring the IRQ state afterwards.
        /// The callback's borrow and native key cannot escape this scope.
        /// Its return value is passed through unchanged.
        ///
        /// # Panics
        /// Panics on reentry after rolling back the failed attempt's key.
        #[inline]
        pub fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
            let _lease = Lease::try_acquire(&self.claimed).expect("SpinMutex is already borrowed");
            // SAFETY: this lease owns the only claim. The callback's output
            // is independent of this borrow, and cleanup is private.
            f(unsafe { &mut *self.inner.get() })
        }

        /// Get a mutable reference without taking the lock. Sound
        /// because `&mut self` proves there are no other handles.
        #[inline]
        pub fn get_mut(&mut self) -> &mut T {
            // SAFETY: `&mut self` is exclusive.
            unsafe { &mut *self.inner.get() }
        }
    }

    struct Lease<'a> {
        claimed: &'a AtomicBool,
        key: bindings::ove_irq_key_t,
        _not_send: PhantomData<*const ()>,
    }

    impl<'a> Lease<'a> {
        #[inline]
        fn try_acquire(claimed: &'a AtomicBool) -> Option<Self> {
            // SAFETY: native context eligibility belongs to the IRQ provider;
            // this synchronous scope retains its key until matching cleanup.
            let key = unsafe { bindings::ove_irq_lock() };
            if claimed
                .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                // SAFETY: retire only this failed attempt, preserving the
                // successful owner's claim and earlier native key.
                unsafe { bindings::ove_irq_unlock(key) };
                return None;
            }
            Some(Self {
                claimed,
                key,
                _not_send: PhantomData,
            })
        }
    }

    impl Drop for Lease<'_> {
        #[inline]
        fn drop(&mut self) {
            self.claimed.store(false, Ordering::Release);
            // SAFETY: the callback borrow ended, and this private lease
            // retires its own key on the acquiring thread in reverse order.
            unsafe { bindings::ove_irq_unlock(self.key) }
        }
    }
}
