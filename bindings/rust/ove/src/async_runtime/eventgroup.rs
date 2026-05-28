// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async wrapper around [`crate::EventGroup`] using
//! `ove_eventgroup_set_notify`.

use core::future::poll_fn;
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::{Error, Result};
use crate::eventgroup::{EventGroup, WaitFlags};

/// Async wrapper around an [`ove::EventGroup`](crate::EventGroup).
pub struct AsyncEventGroup {
    inner: EventGroup,
    waker: AtomicWaker,
}

// SAFETY: `AsyncEventGroup` wraps an `ove_event_group_t` FFI handle.  The
// substrate serialises concurrent waiters via its own internal lock, so
// cross-thread `&self` access is sound.
unsafe impl Send for AsyncEventGroup {}
unsafe impl Sync for AsyncEventGroup {}

impl AsyncEventGroup {
    pub const fn new(inner: EventGroup) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    pub fn arm(&'static self) -> Result<()> {
        unsafe {
            self.inner.set_notify(
                Some(eg_notify_trampoline),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )
        }
    }

    /// Await one or more of the bits in `mask`. Returns the bit pattern
    /// that satisfied the wait.
    pub async fn wait_bits(&'static self, mask: u32, flags: WaitFlags) -> Result<u32> {
        poll_fn(|cx| {
            match self.inner.try_wait_bits(mask, flags) {
                Ok(v) => return Poll::Ready(Ok(v)),
                Err(Error::WouldBlock) | Err(Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            self.waker.register(cx.waker());
            match self.inner.try_wait_bits(mask, flags) {
                Ok(v) => Poll::Ready(Ok(v)),
                Err(Error::WouldBlock) | Err(Error::Timeout) => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    #[inline]
    pub fn inner(&self) -> &EventGroup {
        &self.inner
    }
}

unsafe extern "C" fn eg_notify_trampoline(user_data: *mut core::ffi::c_void) {
    let waker = unsafe { &*(user_data as *const AtomicWaker) };
    waker.wake();
}
