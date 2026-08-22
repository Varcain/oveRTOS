// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Synchronization primitives for oveRTOS.
//!
//! Provides RAII wrappers for mutexes, recursive mutexes, semaphores, events,
//! and condition variables. All types implement `Send + Sync` and work in both
//! heap and zero-heap modes.

use core::cell::UnsafeCell;
use core::fmt;
use core::marker::PhantomData;
use core::mem::ManuallyDrop;
use core::mem::MaybeUninit;
use core::ops::{Deref, DerefMut};

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

// ---------------------------------------------------------------------------
// Guard !Send sentinel
// ---------------------------------------------------------------------------
//
// `MutexGuard` / `RecursiveMutexGuard` must be `!Send` so a guard cannot be
// sent to another thread and unlocked there — backend-defined UB (POSIX
// returns `EPERM`, FreeRTOS asserts in debug / silently releases in release).
//
// `std::sync::MutexGuard` declares `impl !Send for MutexGuard {}` using the
// nightly-only `negative_impls` feature.  On stable, the canonical workaround
// is to embed a `!Send` marker type via `PhantomData`.  This is what
// `lock_api`, `parking_lot`, and `tokio::sync` all do.  Raw pointers are
// `!Send + !Sync`, so this newtype propagates both.
#[allow(dead_code)] // marker type — field is never read by design
struct GuardNoSend(*mut ());

// ---------------------------------------------------------------------------
// Mutex<T>
// ---------------------------------------------------------------------------

/// Mutex protecting a value of type `T`.  `std::sync::Mutex<T>` /
/// `parking_lot::Mutex<T>` analog.
///
/// The lock and the data live together in one type.  Acquiring the
/// lock via [`Mutex::lock`] returns a [`MutexGuard<T>`] that
/// [`Deref`]s to `T`, so the only way to touch the data is to hold
/// the lock first — the borrow checker enforces the contract that
/// `lock` then `*g = …` is the only legal access pattern.
///
/// # Differences from `std::sync::Mutex`
///
/// - No [poisoning](https://doc.rust-lang.org/std/sync/struct.Mutex.html#poisoning).
///   `lock()` returns `Result<MutexGuard<T>, Error>`, not std's
///   `LockResult<MutexGuard<T>>`.  Mirrors parking_lot's choice
///   — forcing callers to unwrap a never-firing `PoisonError` is
///   worse ergonomics than reporting only the backend errors that
///   can actually occur.
/// - [`try_lock_for`](Self::try_lock_for) /
///   [`try_lock_until`](Self::try_lock_until) variants for bounded
///   waits.  Parking_lot has these; std does not.  Return
///   `Result<MutexGuard, Error>` (with [`Error::Timeout`] on
///   deadline elapsed), not parking_lot's `Option<MutexGuard>` —
///   we have backend errors that aren't simply "could not acquire".
pub struct Mutex<T: ?Sized> {
    handle: bindings::ove_mutex_t,
    data: UnsafeCell<T>,
}

// SAFETY: `Mutex<T>` synchronises access to `T`; `T: Send` is enough
// (`Send` is what is needed to ship across thread boundaries, and the
// mutex provides the cross-thread mutual exclusion).  `T: Sync` is NOT
// required — the mutex itself provides the synchronisation that would
// otherwise be `T`'s responsibility.  Matches std / parking_lot.
unsafe impl<T: ?Sized + Send> Send for Mutex<T> {}
unsafe impl<T: ?Sized + Send> Sync for Mutex<T> {}

/// Caller-owned backing storage for a [`Mutex`] in zero-heap mode.  Declare
/// it in a `static` and hand `&STORAGE` to [`Mutex::create`]; in heap mode the
/// storage is ignored.  Mirrors [`crate::ThreadStorage`] — `const fn new()`
/// makes it usable from a `static`, and the bytes are only ever handed to C as
/// a raw pointer (the kernel owns the object's synchronisation).
// The field is only addressed via a raw pointer handed to C, so it reads as
// "never read" in heap mode — same as ClientStorage.
#[allow(dead_code)]
pub struct MutexStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_mutex_storage_t>>,
}

impl MutexStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for MutexStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: the storage is only ever addressed as a raw pointer passed to C;
// the kernel owns the mutex's synchronisation.  Mirrors ThreadStorage's Sync.
unsafe impl Sync for MutexStorage {}

impl<T> Mutex<T> {
    /// Create a new mutex around `val` via heap allocation (heap mode only).
    #[cfg(not(zero_heap))]
    pub fn new(val: T) -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mutex_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            data: UnsafeCell::new(val),
        })
    }

    /// Create a mutex around `val` using caller-provided static storage
    /// for the handle.  Available in zero-heap mode.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Mutex` and is not
    /// shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_mutex_storage_t, val: T) -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mutex_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            data: UnsafeCell::new(val),
        })
    }

    /// Create a mutex around `val` that works in **both** heap and zero-heap
    /// modes: heap mode ignores `storage` and allocates; zero-heap mode backs
    /// the handle with the caller-provided `storage` (which must outlive the
    /// `Mutex`).  This is the mode-agnostic constructor downstream crates use
    /// — `new`/`from_static` are gated to one mode and `from_static` needs the
    /// crate-private storage type, so neither is callable portably.
    pub fn create(storage: &'static MutexStorage, val: T) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new(val)
        }
        #[cfg(zero_heap)]
        {
            // SAFETY: `storage` is 'static and, by this `&'static` borrow of a
            // dedicated `static`, uniquely backs this one Mutex.
            let ptr = UnsafeCell::raw_get(&storage.storage).cast();
            unsafe { Self::from_static(ptr, val) }
        }
    }

    /// Consume the mutex and return the protected value.
    ///
    /// Available in heap mode only (zero-heap mode lacks a destroy path
    /// for the handle — the handle's storage outlives the `Mutex`
    /// anyway).
    #[cfg(not(zero_heap))]
    pub fn into_inner(self) -> Result<T> {
        // ManuallyDrop prevents the regular Drop from running (which
        // would both destroy the handle AND drop the data).  We then
        // extract T and destroy the handle ourselves.
        let this = ManuallyDrop::new(self);
        // SAFETY: `this` is consumed; nothing else accesses `data`.
        let data = unsafe { core::ptr::read(&this.data) };
        unsafe { bindings::ove_mutex_destroy(this.handle) };
        Ok(data.into_inner())
    }
}

impl<T: ?Sized> Mutex<T> {
    /// Get a mutable reference to the protected value without locking.
    ///
    /// Safe because `&mut self` proves the caller has exclusive access
    /// — no other thread can hold a guard at this moment.  Matches
    /// `std::sync::Mutex::get_mut`.
    pub fn get_mut(&mut self) -> &mut T {
        // SAFETY: `&mut self` is exclusive; no aliasing.
        unsafe { &mut *self.data.get() }
    }

    /// Acquire the mutex, blocking indefinitely.  Returns an RAII guard
    /// that releases the lock on drop.
    ///
    /// # Errors
    /// Returns the substrate's error code if the mutex handle is invalid
    /// (programming error — same failure mode as in C/C++).
    #[inline]
    pub fn lock(&self) -> Result<MutexGuard<'_, T>> {
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, u64::MAX) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the mutex without blocking.
    /// `std::sync::Mutex::try_lock` analog.
    ///
    /// # Errors
    /// Returns [`Error::WouldBlock`] if the mutex is currently held by
    /// another thread.
    #[inline]
    pub fn try_lock(&self) -> Result<MutexGuard<'_, T>> {
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, 0) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the mutex, waiting up to `d`.
    /// `parking_lot::Mutex::try_lock_for` analog.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the lock cannot be acquired within
    /// the duration.
    #[inline]
    pub fn try_lock_for(&self, d: core::time::Duration) -> Result<MutexGuard<'_, T>> {
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, crate::time::dur_to_ns(d)) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the mutex by the given deadline.
    /// `parking_lot::Mutex::try_lock_until` analog.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the deadline elapses before the lock
    /// is acquired.
    #[inline]
    pub fn try_lock_until(&self, deadline: crate::time::Instant) -> Result<MutexGuard<'_, T>> {
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, timeout) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }
}

impl<T: ?Sized> Drop for Mutex<T> {
    fn drop(&mut self) {
        if self.handle.is_null() {
            return;
        }
        // `UnsafeCell<T>`'s Drop runs automatically and drops `T`.
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_mutex_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_mutex_deinit(self.handle);
        }
    }
}

impl<T: ?Sized + fmt::Debug> fmt::Debug for Mutex<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Don't try to lock — Debug shouldn't deadlock.  Show only the
        // handle; matches std's "Mutex { data: <locked> }" shape minus
        // the racy data probe.
        f.debug_struct("Mutex")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish_non_exhaustive()
    }
}

/// RAII guard that unlocks a `Mutex<T>` when dropped.
///
/// Deref / DerefMut expose the protected value.  `MutexGuard` is
/// `!Send`: the locking thread must release the lock.  Sending a guard
/// to another thread would cause that thread to issue
/// `ove_mutex_unlock` with no matching `ove_mutex_lock`, which is
/// backend-defined UB.  The `_no_send` field carries a `GuardNoSend`
/// `PhantomData` to propagate `!Send` at compile time (the same trick
/// `lock_api` / `parking_lot` use on stable Rust).
///
/// `Sync` is reinstated for `T: Sync` — a `&MutexGuard<T>` is the same
/// as a `&T`, which is `Send` if `T: Sync`.  Matches std and
/// parking_lot.
pub struct MutexGuard<'a, T: ?Sized> {
    mutex: &'a Mutex<T>,
    _no_send: PhantomData<GuardNoSend>,
}

// SAFETY: `&MutexGuard<T>` is `&T` once `Deref`-ed; if `T: Sync` then
// sharing `&T` across threads is sound.  std / parking_lot do the same.
unsafe impl<T: ?Sized + Sync> Sync for MutexGuard<'_, T> {}

impl<T: ?Sized> Deref for MutexGuard<'_, T> {
    type Target = T;

    #[inline]
    fn deref(&self) -> &T {
        // SAFETY: holding the guard implies the lock is held; we have
        // exclusive read access.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T: ?Sized> DerefMut for MutexGuard<'_, T> {
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: holding the guard implies the lock is held; `&mut self`
        // ensures no other guard exists, so this is the only `&mut T`.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T: ?Sized + fmt::Debug> fmt::Debug for MutexGuard<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // `&&**self` reaches through Deref to `&T`, then a second
        // `&` makes the result Sized so `field(...)` can erase it
        // through `&dyn Debug`.  Mirrors std::sync::MutexGuard's
        // Debug impl.
        f.debug_struct("MutexGuard")
            .field("data", &&**self)
            .finish()
    }
}

impl<T: ?Sized + fmt::Display> fmt::Display for MutexGuard<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        (**self).fmt(f)
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: handle is alive as long as the `&Mutex<T>` is borrowed,
        // which the `'a` lifetime guarantees.
        unsafe {
            bindings::ove_mutex_unlock(self.mutex.handle);
        }
    }
}

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

/// Caller-owned storage for a [`RecursiveMutex`] in zero-heap mode (see
/// [`MutexStorage`]).
#[allow(dead_code)]
pub struct RecursiveMutexStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_mutex_storage_t>>,
}

impl RecursiveMutexStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for RecursiveMutexStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see MutexStorage.
unsafe impl Sync for RecursiveMutexStorage {}

/// RAII wrapper around a recursive mutex.
pub struct RecursiveMutex {
    handle: bindings::ove_mutex_t,
}

impl RecursiveMutex {
    /// Create a new recursive mutex via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_recursive_mutex_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `RecursiveMutex` and is not
    /// shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_mutex_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_recursive_mutex_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Mode-agnostic constructor (see [`Mutex::create`]).
    pub fn create(storage: &'static RecursiveMutexStorage) -> Result<Self> {
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

    /// Acquire the recursive mutex, blocking indefinitely.  Returns an
    /// RAII guard that releases one level of the lock on drop.  Same
    /// thread may call this multiple times — each guard releases one
    /// level when dropped.
    #[inline]
    pub fn lock(&self) -> Result<RecursiveMutexGuard<'_>> {
        let rc = unsafe { bindings::ove_recursive_mutex_lock(self.handle, u64::MAX) };
        Error::from_code(rc)?;
        Ok(RecursiveMutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the recursive mutex without blocking.
    ///
    /// # Errors
    /// Returns [`Error::WouldBlock`] if held by a different thread.
    #[inline]
    pub fn try_lock(&self) -> Result<RecursiveMutexGuard<'_>> {
        let rc = unsafe { bindings::ove_recursive_mutex_lock(self.handle, 0) };
        Error::from_code(rc)?;
        Ok(RecursiveMutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the recursive mutex, waiting up to `d`.
    #[inline]
    pub fn try_lock_for(&self, d: core::time::Duration) -> Result<RecursiveMutexGuard<'_>> {
        let rc =
            unsafe { bindings::ove_recursive_mutex_lock(self.handle, crate::time::dur_to_ns(d)) };
        Error::from_code(rc)?;
        Ok(RecursiveMutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the recursive mutex by the given deadline.
    /// Use [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    #[inline]
    pub fn try_lock_until(
        &self,
        deadline: crate::time::Instant,
    ) -> Result<RecursiveMutexGuard<'_>> {
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc = unsafe { bindings::ove_recursive_mutex_lock(self.handle, timeout) };
        Error::from_code(rc)?;
        Ok(RecursiveMutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Release one level of the recursive lock.
    ///
    /// Prefer letting the RAII guard ([`RecursiveMutexGuard`]) drop
    /// release the lock; this is kept `pub` for parity with the C API.
    #[inline]
    pub fn unlock(&self) {
        unsafe { bindings::ove_recursive_mutex_unlock(self.handle) }
    }
}

crate::ove_handle_impl!(
    RecursiveMutex,
    ove_recursive_mutex_destroy,
    ove_recursive_mutex_deinit
);

/// RAII guard that unlocks a `RecursiveMutex` when dropped.
///
/// Same `!Send` constraint as [`MutexGuard`]: the locking thread must
/// issue the matching unlock.
pub struct RecursiveMutexGuard<'a> {
    mutex: &'a RecursiveMutex,
    _no_send: PhantomData<GuardNoSend>,
}

impl fmt::Debug for RecursiveMutexGuard<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("RecursiveMutexGuard")
            .field("mutex", &format_args!("{:p}", self.mutex.handle))
            .finish()
    }
}

impl Drop for RecursiveMutexGuard<'_> {
    fn drop(&mut self) {
        self.mutex.unlock();
    }
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

/// Caller-owned backing storage for a [`Semaphore`] in zero-heap mode.  See
/// [`MutexStorage`] for the pattern; pass `&STORAGE` to [`Semaphore::create`].
#[allow(dead_code)]
pub struct SemaphoreStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_sem_storage_t>>,
}

impl SemaphoreStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for SemaphoreStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see MutexStorage.
unsafe impl Sync for SemaphoreStorage {}

/// Counting semaphore.
pub struct Semaphore {
    handle: bindings::ove_sem_t,
}

impl Semaphore {
    /// Create a counting semaphore via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new(initial: u32, max: u32) -> Result<Self> {
        let mut handle: bindings::ove_sem_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_sem_create(&mut handle, initial, max) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Semaphore`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_sem_storage_t,
        initial: u32,
        max: u32,
    ) -> Result<Self> {
        let mut handle: bindings::ove_sem_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_sem_init(&mut handle, storage, initial, max) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create a counting semaphore that works in **both** heap and zero-heap
    /// modes (see [`Mutex::create`]).  Heap mode ignores `storage`; zero-heap
    /// mode backs the handle with the caller-provided `storage`.
    pub fn create(storage: &'static SemaphoreStorage, initial: u32, max: u32) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new(initial, max)
        }
        #[cfg(zero_heap)]
        {
            // SAFETY: see Mutex::create.
            let ptr = UnsafeCell::raw_get(&storage.storage).cast();
            unsafe { Self::from_static(ptr, initial, max) }
        }
    }

    /// Acquire one permit, blocking indefinitely.  `tokio::sync::Semaphore::acquire`
    /// analog (sans `.await`).
    #[inline]
    pub fn acquire(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_sem_take(self.handle, u64::MAX) };
        Error::from_code(rc)
    }

    /// Attempt to acquire one permit without blocking.
    ///
    /// # Errors
    /// Returns [`Error::WouldBlock`] if no permit is available.
    #[inline]
    pub fn try_acquire(&self) -> Result<()> {
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
        let rc = unsafe { bindings::ove_sem_take(self.handle, crate::time::dur_to_ns(d)) };
        Error::from_code(rc)
    }

    /// Attempt to acquire one permit by the given deadline.
    /// Use [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    #[inline]
    pub fn try_acquire_until(&self, deadline: crate::time::Instant) -> Result<()> {
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc = unsafe { bindings::ove_sem_take(self.handle, timeout) };
        Error::from_code(rc)
    }

    /// Release one permit.  `tokio::sync::Semaphore::add_permits(1)` /
    /// `embassy_sync::Semaphore::release(1)` analog.
    #[inline]
    pub fn release(&self) {
        unsafe { bindings::ove_sem_give(self.handle) }
    }

    /// Release `n` permits.  Binding-side loop — substrate currently
    /// has no `ove_sem_give_n`, so this calls `ove_sem_give` `n` times.
    #[inline]
    pub fn release_n(&self, n: u32) {
        for _ in 0..n {
            unsafe { bindings::ove_sem_give(self.handle) }
        }
    }

    /// Register a notify callback fired after every successful release.
    /// Wraps the C-level `ove_sem_set_notify`.
    ///
    /// # Safety
    /// Same as [`crate::Stream::set_notify`]: `user_data` must remain
    /// valid for as long as the callback may fire, and `cb` must be
    /// ISR-safe.
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        let rc = unsafe { bindings::ove_sem_set_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}

crate::ove_handle_impl!(Semaphore, ove_sem_destroy, ove_sem_deinit);

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

/// Caller-owned storage for an [`Event`] in zero-heap mode (see [`MutexStorage`]).
#[allow(dead_code)]
pub struct EventStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_event_storage_t>>,
}

impl EventStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for EventStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see MutexStorage.
unsafe impl Sync for EventStorage {}

/// Binary event (signal/wait).
pub struct Event {
    handle: bindings::ove_event_t,
}

impl Event {
    /// Create a new event via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_event_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_event_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Event`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_event_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_event_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_event_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Mode-agnostic constructor (see [`Mutex::create`]).
    pub fn create(storage: &'static EventStorage) -> Result<Self> {
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

    /// Block indefinitely until the event is signalled.
    #[inline]
    pub fn wait(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_event_wait(self.handle, u64::MAX) };
        Error::from_code(rc)
    }

    /// Non-blocking check.
    ///
    /// # Errors
    /// Returns [`Error::WouldBlock`] if the event is not currently
    /// signalled.
    #[inline]
    pub fn try_wait(&self) -> Result<()> {
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
        let rc = unsafe { bindings::ove_event_wait(self.handle, crate::time::dur_to_ns(d)) };
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
    pub fn wait_until(&self, deadline: crate::time::Instant) -> Result<()> {
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc = unsafe { bindings::ove_event_wait(self.handle, timeout) };
        Error::from_code(rc)
    }

    /// Signal the event.
    #[inline]
    pub fn signal(&self) {
        unsafe { bindings::ove_event_signal(self.handle) }
    }

    /// Signal the event from an ISR context.
    #[inline]
    pub fn signal_from_isr(&self) {
        unsafe { bindings::ove_event_signal_from_isr(self.handle) }
    }
}

crate::ove_handle_impl!(Event, ove_event_destroy, ove_event_deinit);

// ---------------------------------------------------------------------------
// CondVar + WaitTimeoutResult
// ---------------------------------------------------------------------------

/// Result of a `Condvar::wait_for` / `wait_until` call indicating
/// whether the wait elapsed without being signalled.
///
/// Matches `std::sync::WaitTimeoutResult` exactly — same type name,
/// same `timed_out()` accessor.  Returned wrapped in a tuple alongside
/// the re-acquired guard.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WaitTimeoutResult {
    timed_out: bool,
}

impl WaitTimeoutResult {
    /// Returns `true` if the wait elapsed without being signalled.
    #[inline]
    pub fn timed_out(&self) -> bool {
        self.timed_out
    }
}

/// Condition variable.  Pair with [`Mutex<T>`] to wait on state changes.
///
/// API matches `parking_lot::Condvar` shape — `wait` consumes the
/// guard, returns it re-acquired.  Predicate variants
/// (`wait_while*`) loop internally so callers can't accidentally write
/// the buggy `if cv.wait_for(...) ... && ready` pattern.
/// Caller-owned storage for a [`CondVar`] in zero-heap mode (see [`MutexStorage`]).
#[allow(dead_code)]
pub struct CondVarStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_condvar_storage_t>>,
}

impl CondVarStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for CondVarStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see MutexStorage.
unsafe impl Sync for CondVarStorage {}

pub struct CondVar {
    handle: bindings::ove_condvar_t,
}

impl CondVar {
    /// Create a new condition variable via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_condvar_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_condvar_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `CondVar`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_condvar_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_condvar_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_condvar_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Mode-agnostic constructor (see [`Mutex::create`]).
    pub fn create(storage: &'static CondVarStorage) -> Result<Self> {
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

    /// Atomically release the guarded mutex and block indefinitely
    /// until signalled.  On return, the mutex is re-acquired and the
    /// guard handed back.  `std::sync::Condvar::wait` analog.
    ///
    /// Always re-check the predicate in a loop after this returns —
    /// spurious wake-ups are permitted by the substrate.  Or use
    /// [`wait_while`](Self::wait_while) which does the loop for you.
    #[inline]
    pub fn wait<'a, T: ?Sized>(&self, guard: MutexGuard<'a, T>) -> Result<MutexGuard<'a, T>> {
        let mutex = guard.mutex;
        // Skip the guard's Drop — substrate atomically releases and
        // re-acquires the mutex internally.
        let _suppress = ManuallyDrop::new(guard);
        let rc = unsafe { bindings::ove_condvar_wait(self.handle, mutex.handle, u64::MAX) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex,
            _no_send: PhantomData,
        })
    }

    /// Atomically release the guarded mutex and wait up to `d`.  On
    /// return, the mutex is re-acquired.  `parking_lot::Condvar::wait_for`
    /// analog.
    ///
    /// The returned [`WaitTimeoutResult`] distinguishes "signalled in
    /// time" (`timed_out() == false`) from "elapsed without signal"
    /// (`timed_out() == true`).
    ///
    /// # Errors
    /// Returns an error for backend failures other than a clean timeout
    /// — a clean timeout returns `Ok((guard, WaitTimeoutResult { ..true }))`.
    #[inline]
    pub fn wait_for<'a, T: ?Sized>(
        &self,
        guard: MutexGuard<'a, T>,
        d: core::time::Duration,
    ) -> Result<(MutexGuard<'a, T>, WaitTimeoutResult)> {
        self.wait_with_timeout(guard, crate::time::dur_to_ns(d))
    }

    /// Atomically release the guarded mutex and wait by the given
    /// deadline.  `parking_lot::Condvar::wait_until` analog.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    #[inline]
    pub fn wait_until<'a, T: ?Sized>(
        &self,
        guard: MutexGuard<'a, T>,
        deadline: crate::time::Instant,
    ) -> Result<(MutexGuard<'a, T>, WaitTimeoutResult)> {
        self.wait_with_timeout(guard, crate::time::deadline_to_timeout_ns(deadline))
    }

    #[inline]
    fn wait_with_timeout<'a, T: ?Sized>(
        &self,
        guard: MutexGuard<'a, T>,
        timeout_ns: u64,
    ) -> Result<(MutexGuard<'a, T>, WaitTimeoutResult)> {
        let mutex = guard.mutex;
        let _suppress = ManuallyDrop::new(guard);
        let rc = unsafe { bindings::ove_condvar_wait(self.handle, mutex.handle, timeout_ns) };
        // OVE_ERR_TIMEOUT is the "clean timeout, mutex re-acquired"
        // case — re-package as `Ok((guard, timed_out: true))`.  Other
        // negative codes are real errors.
        let timed_out = match Error::from_code(rc) {
            Ok(()) => false,
            Err(Error::Timeout) => true,
            Err(e) => return Err(e),
        };
        Ok((
            MutexGuard {
                mutex,
                _no_send: PhantomData,
            },
            WaitTimeoutResult { timed_out },
        ))
    }

    /// Loop until `cond(&mut T)` returns `false`, releasing the lock
    /// while waiting.  `std::sync::Condvar::wait_while` analog.
    ///
    /// Internally handles spurious wake-ups by re-checking the
    /// predicate after every wake.
    pub fn wait_while<'a, T: ?Sized, F>(
        &self,
        mut guard: MutexGuard<'a, T>,
        mut cond: F,
    ) -> Result<MutexGuard<'a, T>>
    where
        F: FnMut(&mut T) -> bool,
    {
        while cond(&mut *guard) {
            guard = self.wait(guard)?;
        }
        Ok(guard)
    }

    /// `wait_while` with a duration bound.
    ///
    /// Returns `(guard, WaitTimeoutResult)` — `timed_out()` is `true`
    /// iff the predicate is still `true` at deadline.
    pub fn wait_while_for<'a, T: ?Sized, F>(
        &self,
        guard: MutexGuard<'a, T>,
        d: core::time::Duration,
        cond: F,
    ) -> Result<(MutexGuard<'a, T>, WaitTimeoutResult)>
    where
        F: FnMut(&mut T) -> bool,
    {
        let deadline = crate::time::Instant::now() + d;
        self.wait_while_until(guard, deadline, cond)
    }

    /// `wait_while` with an absolute deadline.
    pub fn wait_while_until<'a, T: ?Sized, F>(
        &self,
        mut guard: MutexGuard<'a, T>,
        deadline: crate::time::Instant,
        mut cond: F,
    ) -> Result<(MutexGuard<'a, T>, WaitTimeoutResult)>
    where
        F: FnMut(&mut T) -> bool,
    {
        loop {
            if !cond(&mut *guard) {
                return Ok((guard, WaitTimeoutResult { timed_out: false }));
            }
            let (g, wtr) = self.wait_until(guard, deadline)?;
            guard = g;
            if wtr.timed_out() {
                // Re-evaluate one final time — the substrate may have
                // released the lock right at the deadline; cond may now
                // be false.  Matches std's semantics.
                let timed_out = cond(&mut *guard);
                return Ok((guard, WaitTimeoutResult { timed_out }));
            }
        }
    }

    /// Wake one waiter.
    #[inline]
    pub fn signal(&self) {
        unsafe { bindings::ove_condvar_signal(self.handle) }
    }

    /// Wake all waiters.
    #[inline]
    pub fn broadcast(&self) {
        unsafe { bindings::ove_condvar_broadcast(self.handle) }
    }
}

crate::ove_handle_impl!(CondVar, ove_condvar_destroy, ove_condvar_deinit);

// ── SpinMutex (G5) ───────────────────────────────────────────────────
//
// IRQ-locking mutex for very short critical sections. Distinct from
// the sleeping `Mutex<T>` above:
//
// - `Mutex::lock` blocks the calling thread on a kernel mutex; legal
//   from thread context only.
// - `SpinMutex::lock` disables interrupts globally and holds them
//   disabled for the body of the closure / lifetime of the guard. Use
//   for sub-microsecond updates to shared state that must be readable
//   from ISR context, or to bridge `&` data into an async task that
//   can't hold a real mutex across an `.await`.
//
// Requires the async substrate (`CONFIG_OVE_ASYNC=y`) — `ove_irq_lock`
// lives in `include/ove/irq.h` under that gate. Not available on WASM
// (no interrupt model). Consumers that need to be portable across
// builds with `CONFIG_OVE_ASYNC=n` should gate with
// `#[cfg(all(has_async, not(board_wasm)))]`.

#[cfg(all(has_async, not(board_wasm)))]
pub use spin_mutex::{SpinMutex, SpinMutexGuard};

#[cfg(all(has_async, not(board_wasm)))]
mod spin_mutex {
    use ::core::cell::UnsafeCell;
    use ::core::ops::{Deref, DerefMut};

    use crate::bindings;

    /// IRQ-locking mutex for very short critical sections.
    ///
    /// Acquiring the lock disables interrupts globally for the lifetime
    /// of the [`SpinMutexGuard`]; releasing it restores the previous
    /// interrupt state. **Do not hold across blocking calls or `.await`
    /// points** — the system will deadlock if a higher-priority
    /// interrupt is needed to make progress.
    ///
    /// Maps to `ove_irq_lock` / `ove_irq_unlock` on the C side, which
    /// is itself a thin wrapper over the host RTOS's
    /// interrupt-disable primitive (`taskENTER_CRITICAL` on FreeRTOS,
    /// `irq_lock()` on Zephyr, `enter_critical_section()` on NuttX,
    /// `pthread_sigmask` on POSIX).
    pub struct SpinMutex<T: ?Sized> {
        // UnsafeCell — `lock()` hands out `&mut T` while interrupts
        // are disabled, which is sound because the disable masks out
        // any other code path that could observe the same `&mut`.
        inner: UnsafeCell<T>,
    }

    // SAFETY: any `T: Send` may be shared between threads because the
    // mutex enforces mutual exclusion via the interrupt-disable.
    unsafe impl<T: ?Sized + Send> Send for SpinMutex<T> {}
    unsafe impl<T: ?Sized + Send> Sync for SpinMutex<T> {}

    impl<T> SpinMutex<T> {
        /// Wrap `value` in a new `SpinMutex`.
        #[inline]
        pub const fn new(value: T) -> Self {
            Self {
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
        /// Acquire the lock by disabling interrupts. The returned
        /// guard restores them when dropped.
        #[inline]
        pub fn lock(&self) -> SpinMutexGuard<'_, T> {
            // SAFETY: ove_irq_lock is safe from any context.
            let key = unsafe { bindings::ove_irq_lock() };
            SpinMutexGuard {
                mtx: self,
                key,
                _not_send: ::core::marker::PhantomData,
            }
        }

        /// Run `f` with the value, restoring the IRQ state afterwards.
        /// Sugar around [`SpinMutex::lock`] that scopes the lock to
        /// the closure body.
        #[inline]
        pub fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
            let mut g = self.lock();
            f(&mut *g)
        }

        /// Get a mutable reference without taking the lock. Sound
        /// because `&mut self` proves there are no other handles.
        #[inline]
        pub fn get_mut(&mut self) -> &mut T {
            // SAFETY: `&mut self` is exclusive.
            unsafe { &mut *self.inner.get() }
        }
    }

    /// RAII guard that releases the [`SpinMutex`] when dropped.
    pub struct SpinMutexGuard<'a, T: ?Sized> {
        mtx: &'a SpinMutex<T>,
        key: bindings::ove_irq_key_t,
        // `*const ()` is !Send + !Sync by default; ensures the guard
        // can't migrate to another thread and call ove_irq_unlock from
        // the wrong context.
        _not_send: ::core::marker::PhantomData<*const ()>,
    }

    impl<T: ?Sized> Deref for SpinMutexGuard<'_, T> {
        type Target = T;
        #[inline]
        fn deref(&self) -> &T {
            // SAFETY: the lock guarantees exclusive access.
            unsafe { &*self.mtx.inner.get() }
        }
    }

    impl<T: ?Sized> DerefMut for SpinMutexGuard<'_, T> {
        #[inline]
        fn deref_mut(&mut self) -> &mut T {
            unsafe { &mut *self.mtx.inner.get() }
        }
    }

    impl<T: ?Sized> Drop for SpinMutexGuard<'_, T> {
        #[inline]
        fn drop(&mut self) {
            // SAFETY: `key` was returned by the matching ove_irq_lock.
            unsafe { bindings::ove_irq_unlock(self.key) }
        }
    }
}
