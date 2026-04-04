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

use crate::bindings;
use crate::error::{Error, Result};

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
    pub unsafe fn from_static(
        storage: *mut bindings::ove_mutex_storage_t,
    ) -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_mutex_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Lock with a timeout in milliseconds. Use `WAIT_FOREVER` for no timeout.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the mutex cannot be acquired within `timeout_ms`.
    pub fn lock(&self, timeout_ms: u32) -> Result<()> {
        let rc = unsafe { bindings::ove_mutex_lock(self.handle, timeout_ms) };
        Error::from_code(rc)
    }

    /// Unlock the mutex.
    pub fn unlock(&self) {
        unsafe { bindings::ove_mutex_unlock(self.handle) }
    }

    /// Lock and return an RAII guard that auto-unlocks on drop.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the lock cannot be acquired within `timeout_ms`.
    pub fn guard(&self, timeout_ms: u32) -> Result<MutexGuard<'_>> {
        self.lock(timeout_ms)?;
        Ok(MutexGuard { mutex: self })
    }

    /// Get the raw handle (for use with CondVar).
    pub(crate) fn raw(&self) -> bindings::ove_mutex_t {
        self.handle
    }
}

crate::ove_handle_impl!(Mutex, ove_mutex_destroy, ove_mutex_deinit);

/// RAII guard that unlocks a `Mutex` when dropped.
pub struct MutexGuard<'a> {
    mutex: &'a Mutex,
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
    pub unsafe fn from_static(
        storage: *mut bindings::ove_mutex_storage_t,
    ) -> Result<Self> {
        let mut handle: bindings::ove_mutex_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_recursive_mutex_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Lock with a timeout in milliseconds.
    ///
    /// The same thread may lock the mutex multiple times; each lock must be
    /// paired with a corresponding [`unlock`](RecursiveMutex::unlock).
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the lock cannot be acquired within `timeout_ms`.
    pub fn lock(&self, timeout_ms: u32) -> Result<()> {
        let rc = unsafe { bindings::ove_recursive_mutex_lock(self.handle, timeout_ms) };
        Error::from_code(rc)
    }

    /// Unlock the recursive mutex.
    pub fn unlock(&self) {
        unsafe { bindings::ove_recursive_mutex_unlock(self.handle) }
    }

    /// Lock and return an RAII guard that auto-unlocks on drop.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the lock cannot be acquired within `timeout_ms`.
    pub fn guard(&self, timeout_ms: u32) -> Result<RecursiveMutexGuard<'_>> {
        self.lock(timeout_ms)?;
        Ok(RecursiveMutexGuard { mutex: self })
    }
}

crate::ove_handle_impl!(RecursiveMutex, ove_recursive_mutex_destroy, ove_mutex_deinit);

/// RAII guard that unlocks a `RecursiveMutex` when dropped.
pub struct RecursiveMutexGuard<'a> {
    mutex: &'a RecursiveMutex,
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

    /// Decrement (take) the semaphore, blocking up to `timeout_ms` if the count is zero.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the semaphore cannot be taken within `timeout_ms`.
    pub fn take(&self, timeout_ms: u32) -> Result<()> {
        let rc = unsafe { bindings::ove_sem_take(self.handle, timeout_ms) };
        Error::from_code(rc)
    }

    /// Post/give the semaphore.
    pub fn give(&self) {
        unsafe { bindings::ove_sem_give(self.handle) }
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
    pub unsafe fn from_static(
        storage: *mut bindings::ove_event_storage_t,
    ) -> Result<Self> {
        let mut handle: bindings::ove_event_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_event_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Block until the event is signalled or the timeout expires.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the event is not signalled within `timeout_ms`.
    pub fn wait(&self, timeout_ms: u32) -> Result<()> {
        let rc = unsafe { bindings::ove_event_wait(self.handle, timeout_ms) };
        Error::from_code(rc)
    }

    /// Signal the event.
    pub fn signal(&self) {
        unsafe { bindings::ove_event_signal(self.handle) }
    }

    /// Signal the event from an ISR context.
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
    pub unsafe fn from_static(
        storage: *mut bindings::ove_condvar_storage_t,
    ) -> Result<Self> {
        let mut handle: bindings::ove_condvar_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_condvar_init(&mut handle, storage) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Atomically release `mutex` and block until signalled or `timeout_ms` elapses.
    ///
    /// On return (successful or not), `mutex` is re-acquired before this function returns.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if neither [`signal`](CondVar::signal) nor
    /// [`broadcast`](CondVar::broadcast) fires within `timeout_ms`.
    pub fn wait(&self, mutex: &Mutex, timeout_ms: u32) -> Result<()> {
        let rc =
            unsafe { bindings::ove_condvar_wait(self.handle, mutex.raw(), timeout_ms) };
        Error::from_code(rc)
    }

    /// Wake one waiter.
    pub fn signal(&self) {
        unsafe { bindings::ove_condvar_signal(self.handle) }
    }

    /// Wake all waiters.
    pub fn broadcast(&self) {
        unsafe { bindings::ove_condvar_broadcast(self.handle) }
    }
}

crate::ove_handle_impl!(CondVar, ove_condvar_destroy, ove_condvar_deinit);
