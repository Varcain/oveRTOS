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

use core::fmt;
use core::marker::PhantomData;

use crate::bindings;
use crate::error::{Error, Result};

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
// Mutex
// ---------------------------------------------------------------------------

/// RAII wrapper around `ove_mutex_t`.
pub struct Mutex {
    handle: bindings::ove_mutex_t,
}

impl Mutex {
    /// Create a new mutex via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mutex_create(&mut handle) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Mutex` and is not
    /// shared with another primitive.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(storage: *mut bindings::ove_mutex_storage_t) -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mutex_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Acquire the mutex, blocking indefinitely.  Returns an RAII guard
    /// that releases the lock on drop.  `std::sync::Mutex::lock` analog
    /// (sans poison).
    ///
    /// # Errors
    /// Returns the substrate's error code if the mutex handle is invalid
    /// (programming error — same failure mode as in C/C++).
    #[inline]
    pub fn lock(&self) -> Result<MutexGuard<'_>> {
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
    pub fn try_lock(&self) -> Result<MutexGuard<'_>> {
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
    pub fn try_lock_for(&self, d: core::time::Duration) -> Result<MutexGuard<'_>> {
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, crate::time::dur_to_ns(d)) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Attempt to acquire the mutex by the given deadline.
    /// `parking_lot::Mutex::try_lock_until` analog.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an indefinite wait.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the deadline elapses before the lock
    /// is acquired.
    #[inline]
    pub fn try_lock_until(&self, deadline: crate::time::Instant) -> Result<MutexGuard<'_>> {
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, timeout) };
        Error::from_code(rc)?;
        Ok(MutexGuard {
            mutex: self,
            _no_send: PhantomData,
        })
    }

    /// Release the mutex.
    ///
    /// Use of the standalone `unlock` is discouraged — prefer the RAII
    /// guard returned by [`lock`](Self::lock) / [`try_lock`](Self::try_lock)
    /// etc.  Kept `pub` because [`CondVar`] needs to drive lock state
    /// through the handle, and so internal-only utilities can release on
    /// reset paths.
    #[inline]
    pub fn unlock(&self) {
        unsafe { bindings::ove_mutex_unlock(self.handle) }
    }

    /// Get the raw handle (for use with CondVar).
    pub(crate) fn raw(&self) -> bindings::ove_mutex_t {
        self.handle
    }
}

crate::ove_handle_impl!(Mutex, ove_mutex_destroy, ove_mutex_deinit);

/// RAII guard that unlocks a `Mutex` when dropped.
///
/// `MutexGuard` is `!Send`: the locking thread must release the lock.
/// Sending a guard to another thread would cause that thread to issue
/// `ove_mutex_unlock` with no matching `ove_mutex_lock`, which is
/// backend-defined UB.  The `_no_send` field carries a [`GuardNoSend`]
/// `PhantomData` to propagate `!Send` at compile time.
pub struct MutexGuard<'a> {
    mutex: &'a Mutex,
    _no_send: PhantomData<GuardNoSend>,
}

impl fmt::Debug for MutexGuard<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MutexGuard")
            .field("mutex", &format_args!("{:p}", self.mutex.handle))
            .finish()
    }
}

impl Drop for MutexGuard<'_> {
    fn drop(&mut self) {
        self.mutex.unlock();
    }
}

// ---------------------------------------------------------------------------
// RecursiveMutex
// ---------------------------------------------------------------------------

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
        let rc = unsafe {
            bindings::ove_recursive_mutex_lock(self.handle, crate::time::dur_to_ns(d))
        };
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
    /// As with [`Mutex::unlock`], prefer the RAII guard.  Kept `pub`
    /// for parity.
    #[inline]
    pub fn unlock(&self) {
        unsafe { bindings::ove_recursive_mutex_unlock(self.handle) }
    }
}

crate::ove_handle_impl!(
    RecursiveMutex,
    ove_recursive_mutex_destroy,
    ove_mutex_deinit
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
}

crate::ove_handle_impl!(Semaphore, ove_sem_destroy, ove_sem_deinit);

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

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
// CondVar
// ---------------------------------------------------------------------------

/// Condition variable.
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

    /// Atomically release `mutex` and block indefinitely until
    /// signalled.  On return, `mutex` is re-acquired.
    /// `std::sync::Condvar::wait` analog.
    ///
    /// Always re-check the predicate in a loop after this returns —
    /// spurious wake-ups are permitted by the substrate.  Or use the
    /// `wait_while*` predicate variants in [`B3`'s `Mutex<T>`] redesign
    /// once it lands.
    #[inline]
    pub fn wait(&self, mutex: &Mutex) -> Result<()> {
        let rc =
            unsafe { bindings::ove_condvar_wait(self.handle, mutex.raw(), u64::MAX) };
        Error::from_code(rc)
    }

    /// Atomically release `mutex` and wait up to `d`.  On return,
    /// `mutex` is re-acquired.  `parking_lot::Condvar::wait_for` analog.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if neither [`signal`](CondVar::signal)
    /// nor [`broadcast`](CondVar::broadcast) fires within `d`.
    #[inline]
    pub fn wait_for(&self, mutex: &Mutex, d: core::time::Duration) -> Result<()> {
        let rc = unsafe {
            bindings::ove_condvar_wait(self.handle, mutex.raw(), crate::time::dur_to_ns(d))
        };
        Error::from_code(rc)
    }

    /// Atomically release `mutex` and wait by the given deadline.
    /// `parking_lot::Condvar::wait_until` analog.  Use
    /// [`Instant::FOREVER`](crate::time::Instant::FOREVER) for an
    /// indefinite wait.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the deadline elapses before
    /// [`signal`](CondVar::signal) or [`broadcast`](CondVar::broadcast)
    /// fires.
    #[inline]
    pub fn wait_until(&self, mutex: &Mutex, deadline: crate::time::Instant) -> Result<()> {
        let timeout = crate::time::deadline_to_timeout_ns(deadline);
        let rc = unsafe { bindings::ove_condvar_wait(self.handle, mutex.raw(), timeout) };
        Error::from_code(rc)
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
