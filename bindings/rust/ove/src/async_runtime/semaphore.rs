// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async wrapper around [`crate::Semaphore`] using
//! `ove_sem_set_notify`.

use core::future::poll_fn;
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::{Error, Result};
use crate::sync::Semaphore;

/// Async wrapper around an [`ove::Semaphore`](crate::Semaphore).
///
/// Initialize at its final address, call [`Self::arm`] with exclusive static
/// access, then share the returned reference with producers and async waiters.
pub struct AsyncSemaphore {
    inner: Semaphore,
    waker: AtomicWaker,
}

// SAFETY: `AsyncSemaphore` wraps a `Semaphore` (whose own Send/Sync
// reflects the substrate's atomic counter + waiter list).  The
// `AtomicWaker` is `Sync` by construction. Exclusive setup installs its
// callback before the returned shared reference can be published.
unsafe impl Send for AsyncSemaphore {}
unsafe impl Sync for AsyncSemaphore {}

impl AsyncSemaphore {
    /// Wrap a semaphore without installing its callback. Place this wrapper in
    /// program-lifetime storage before calling [`Self::arm`].
    pub const fn new(inner: Semaphore) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    /// Install the wake callback before sharing this wrapper.
    ///
    /// Exclusive static access prevents registration from overlapping another
    /// registration or a producer. On success, publish the returned shared
    /// reference for [`Self::acquire`] and [`Self::inner`] operations.
    /// An initialization cell that yields `&'static mut Self` can supply the
    /// final address without an allocator or application FFI.
    ///
    /// # Errors
    /// Returns [`Error::NotSupported`] outside an admitted task producer context,
    /// and propagates native registration errors.
    /// The static borrow is consumed even on error: the wrapper and its native
    /// backing stay retained, and safe code cannot retry setup or reclaim them.
    /// Choose an admitted task producer context before calling this method.
    ///
    /// ```no_run
    /// use ove::{AsyncSemaphore, Result};
    /// fn initialize(sem: &'static mut AsyncSemaphore) -> Result<&'static AsyncSemaphore> {
    ///     sem.arm()
    /// }
    /// ```
    ///
    /// Shared access cannot register a callback:
    ///
    /// ```compile_fail,E0596
    /// use ove::AsyncSemaphore;
    /// fn shared(sem: &'static AsyncSemaphore) {
    ///     let _ = sem.arm();
    /// }
    /// ```
    ///
    /// Setup cannot be repeated, including after an error:
    ///
    /// ```compile_fail,E0499
    /// use ove::AsyncSemaphore;
    /// fn twice(sem: &'static mut AsyncSemaphore) {
    ///     let _ = sem.arm();
    ///     let _ = sem.arm();
    /// }
    /// ```
    ///
    /// A producer borrow cannot remain live across setup:
    ///
    /// ```compile_fail,E0502
    /// use ove::AsyncSemaphore;
    /// fn retained_producer(sem: &'static mut AsyncSemaphore) {
    ///     let producer = sem.inner();
    ///     let _ = sem.arm();
    ///     producer.release();
    /// }
    /// ```
    pub fn arm(&'static mut self) -> Result<&'static Self> {
        // SAFETY: exclusive setup excludes safe producers and registrations.
        // The final static address keeps callback data live on every outcome;
        // AtomicWaker supports concurrent callbacks after shared publication.
        unsafe {
            self.inner.set_notify(
                Some(sem_notify_trampoline),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )?;
        }
        Ok(self)
    }

    /// Acquire one permit. Awaits until the count is non-zero.
    pub async fn acquire(&'static self) -> Result<()> {
        poll_fn(|cx| {
            match self.inner.try_acquire() {
                Ok(()) => return Poll::Ready(Ok(())),
                Err(Error::WouldBlock | Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            self.waker.register(cx.waker());
            match self.inner.try_acquire() {
                Ok(()) => Poll::Ready(Ok(())),
                Err(Error::WouldBlock | Error::Timeout) => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    /// Borrow the synchronous semaphore. After setup, producers use this
    /// through the shared reference returned by [`Self::arm`].
    #[inline]
    pub fn inner(&self) -> &Semaphore {
        &self.inner
    }
}

unsafe extern "C" fn sem_notify_trampoline(user_data: *mut core::ffi::c_void) {
    let waker = unsafe { &*(user_data as *const AtomicWaker) };
    waker.wake();
}
