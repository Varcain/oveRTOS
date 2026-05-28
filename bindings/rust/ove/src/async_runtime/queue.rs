// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async wrapper around [`crate::Queue`] using `ove_queue_set_notify`.
//!
//! Same pattern as `AsyncStream`: own the wrapped primitive + an
//! `AtomicWaker`, register a C trampoline that wakes the waker, poll
//! `try_recv` with waker-recheck.
//!
//! Lifetime: methods take `&'static self` because the C-side notify
//! retains a pointer to the internal AtomicWaker.

use core::future::poll_fn;
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::{Error, Result};
use crate::queue::Queue;

/// Async wrapper around an [`ove::Queue`](crate::Queue).
pub struct AsyncQueue<T: Copy, const N: usize> {
    inner: Queue<T, N>,
    waker: AtomicWaker,
}

// SAFETY: `AsyncQueue<T, N>` wraps a `Queue<T, N>` whose own Send/Sync
// impls (see `queue.rs`) reflect the substrate's locking.  The `T: Copy +
// Send` bound guarantees items can cross thread boundaries.
unsafe impl<T: Copy + Send, const N: usize> Send for AsyncQueue<T, N> {}
unsafe impl<T: Copy + Send, const N: usize> Sync for AsyncQueue<T, N> {}

impl<T: Copy, const N: usize> AsyncQueue<T, N> {
    /// Wrap a queue for async use. See [`Self::arm`] for the lifetime
    /// constraint.
    pub const fn new(inner: Queue<T, N>) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    /// Register the C-side notify callback. Must be called exactly once
    /// after the wrapper reaches its final 'static location.
    pub fn arm(&'static self) -> Result<()> {
        unsafe {
            self.inner.set_notify(
                Some(queue_notify_trampoline::<T, N>),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )
        }
    }

    /// Async receive — yields control until an item is available, then
    /// returns it.
    pub async fn recv(&'static self) -> Result<T> {
        poll_fn(|cx| {
            // Fast path
            match self.inner.try_recv() {
                Ok(v) => return Poll::Ready(Ok(v)),
                Err(Error::WouldBlock) | Err(Error::QueueEmpty) | Err(Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            // Register + recheck
            self.waker.register(cx.waker());
            match self.inner.try_recv() {
                Ok(v) => Poll::Ready(Ok(v)),
                Err(Error::WouldBlock) | Err(Error::QueueEmpty) | Err(Error::Timeout) => {
                    Poll::Pending
                }
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    /// Borrow the underlying [`Queue`] for synchronous operations
    /// (`try_send`, etc.).
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
