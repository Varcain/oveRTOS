// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async wrapper around [`crate::Queue`] using `ove_queue_set_notify`.
//!
//! Place the owner and its AtomicWaker at their final address, install the
//! callback through exclusive static access, then share the returned ready
//! reference. Receive registers a waker and rechecks the synchronous queue.

use core::future::poll_fn;
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::{Error, Result};
use crate::queue::Queue;

/// Async wrapper around an [`ove::Queue`](crate::Queue).
/// Initialize at its final address and call [`Self::arm`] before sharing it.
pub struct AsyncQueue<T: Copy, const N: usize> {
    inner: Queue<T, N>,
    waker: AtomicWaker,
}

// SAFETY: `AsyncQueue<T, N>` wraps a `Queue<T, N>` whose own Send/Sync
// impls (see `queue.rs`) reflect the substrate's locking.  The `T: Copy +
// Send` bound guarantees items can cross thread boundaries. AtomicWaker protects
// callback state; exclusive setup installs the hook before shared publication.
unsafe impl<T: Copy + Send, const N: usize> Send for AsyncQueue<T, N> {}
unsafe impl<T: Copy + Send, const N: usize> Sync for AsyncQueue<T, N> {}

impl<T: Copy, const N: usize> AsyncQueue<T, N> {
    /// Wrap a queue without installing its callback. Place this wrapper in
    /// program-lifetime storage before calling [`Self::arm`].
    pub const fn new(inner: Queue<T, N>) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    /// Install the wake callback before sharing this wrapper.
    ///
    /// Exclusive static access excludes other registrations and live producer
    /// borrows. On success, publish the returned shared reference for receive
    /// and synchronous [`Self::inner`] operations. A cell such as StaticCell
    /// yielding `&'static mut Self` supplies final placement without application
    /// FFI or an allocator.
    ///
    /// # Errors
    /// Returns [`Error::NotSupported`] when native RESOURCE setup is unavailable
    /// and propagates native registration errors. Producer-only masked contexts
    /// do not admit this setup, which can take a native mutex.
    /// The static borrow is consumed even on error: wrapper and native backing
    /// remain retained, and safe code cannot retry setup or reclaim them.
    /// Choose an admitted resource context before calling this method.
    ///
    /// ```no_run
    /// use ove::{AsyncQueue, Result};
    /// fn initialize(q: &'static mut AsyncQueue<u32, 2>) -> Result<&'static AsyncQueue<u32, 2>> {
    ///     q.arm()
    /// }
    /// ```
    ///
    /// Shared access cannot register:
    ///
    /// ```compile_fail,E0596
    /// use ove::AsyncQueue;
    /// fn shared(q: &'static AsyncQueue<u32, 2>) {
    ///     let _ = q.arm();
    /// }
    /// ```
    ///
    /// Setup cannot be repeated:
    ///
    /// ```compile_fail,E0499
    /// use ove::AsyncQueue;
    /// fn twice(q: &'static mut AsyncQueue<u32, 2>) {
    ///     let _ = q.arm();
    ///     let _ = q.arm();
    /// }
    /// ```
    ///
    /// Error does not release the static borrow:
    ///
    /// ```compile_fail,E0499
    /// use ove::AsyncQueue;
    /// fn retry(q: &'static mut AsyncQueue<u32, 2>) {
    ///     if q.arm().is_err() { let _ = q.arm(); }
    /// }
    /// ```
    ///
    /// A producer borrow cannot remain live across setup:
    ///
    /// ```compile_fail,E0502
    /// use ove::AsyncQueue;
    /// fn producer(q: &'static mut AsyncQueue<u32, 2>) {
    ///     let producer = q.inner();
    ///     let _ = q.arm();
    ///     let _ = producer.try_send(&1);
    /// }
    /// ```
    pub fn arm(&'static mut self) -> Result<&'static Self> {
        // SAFETY: exclusive setup excludes safe producers and registration.
        // The final static address retains the waker on every outcome; its
        // interior mutability supports callbacks after shared publication.
        unsafe {
            self.inner.set_notify(
                Some(queue_notify_trampoline::<T, N>),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )?;
        }
        Ok(self)
    }

    /// Async receive — yields control until an item is available, then
    /// returns it.
    pub async fn recv(&'static self) -> Result<T> {
        poll_fn(|cx| {
            // Fast path
            match self.inner.try_recv() {
                Ok(v) => return Poll::Ready(Ok(v)),
                Err(Error::WouldBlock | Error::QueueEmpty | Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            // Register + recheck
            self.waker.register(cx.waker());
            match self.inner.try_recv() {
                Ok(v) => Poll::Ready(Ok(v)),
                Err(Error::WouldBlock | Error::QueueEmpty | Error::Timeout) => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    /// Borrow the underlying [`Queue`] for synchronous operations
    /// (`try_send`, etc.).
    /// After setup, use the shared reference returned by [`Self::arm`].
    #[inline]
    pub fn inner(&self) -> &Queue<T, N> {
        &self.inner
    }
}

unsafe extern "C" fn queue_notify_trampoline<T: Copy, const N: usize>(
    user_data: *mut core::ffi::c_void,
) {
    let waker = unsafe { &*(user_data as *const AtomicWaker) };
    waker.wake();
}
