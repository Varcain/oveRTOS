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
pub struct AsyncSemaphore {
    inner: Semaphore,
    waker: AtomicWaker,
}

unsafe impl Send for AsyncSemaphore {}
unsafe impl Sync for AsyncSemaphore {}

impl AsyncSemaphore {
    pub const fn new(inner: Semaphore) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    pub fn arm(&'static self) -> Result<()> {
        unsafe {
            self.inner.set_notify(
                Some(sem_notify_trampoline),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )
        }
    }

    /// Acquire one permit. Awaits until the count is non-zero.
    pub async fn acquire(&'static self) -> Result<()> {
        poll_fn(|cx| {
            match self.inner.try_acquire() {
                Ok(()) => return Poll::Ready(Ok(())),
                Err(Error::WouldBlock) | Err(Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            self.waker.register(cx.waker());
            match self.inner.try_acquire() {
                Ok(()) => Poll::Ready(Ok(())),
                Err(Error::WouldBlock) | Err(Error::Timeout) => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    #[inline]
    pub fn inner(&self) -> &Semaphore {
        &self.inner
    }
}

unsafe extern "C" fn sem_notify_trampoline(user_data: *mut core::ffi::c_void) {
    let waker = unsafe { &*(user_data as *const AtomicWaker) };
    waker.wake();
}
