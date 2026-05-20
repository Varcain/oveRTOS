// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async wrapper around [`crate::Uart`] using `ove_uart_set_rx_notify`,
//! which delegates to the underlying RX stream's notify hook.
//!
//! The UART driver pushes received bytes into an internal `ove_stream`
//! from ISR context via `ove_uart_rx_isr_push`. Registering a notify
//! callback there is exactly what async RX needs: each pushed byte
//! triggers the trampoline, which wakes the embassy task suspended on
//! `read().await`.
//!
//! TX is left synchronous — `Uart::write` already blocks until the
//! requested bytes are accepted into the HW FIFO, and exposing a true
//! async TX would need a separate TX-completion DMA callback path
//! (Phase 3 territory).

use core::future::poll_fn;
use core::task::Poll;
use core::time::Duration;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::{Error, Result};
use crate::uart::Uart;

/// Async wrapper around an [`ove::Uart`](crate::Uart).
///
/// Same lifetime contract as the other `Async*` wrappers: methods take
/// `&'static self` because the C-side notify callback retains a
/// pointer to the internal `AtomicWaker`.
pub struct AsyncUart {
    inner: Uart,
    waker: AtomicWaker,
}

unsafe impl Send for AsyncUart {}
unsafe impl Sync for AsyncUart {}

impl AsyncUart {
    pub const fn new(inner: Uart) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    /// Register the C-side notify callback. Must be called exactly once
    /// after the wrapper reaches its final 'static location.
    pub fn arm(&'static self) -> Result<()> {
        // SAFETY: 'static wrapper, AtomicWaker pinned for program
        // lifetime; trampoline performs an ISR-safe AtomicWaker::wake.
        unsafe {
            self.inner.set_rx_notify(
                Some(uart_notify_trampoline),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )
        }
    }

    /// Async read — awaits until at least one byte is available, then
    /// returns the number of bytes copied.
    ///
    /// Returns `Ok(0)` only if `buf.is_empty()`.
    pub async fn read(&'static self, buf: &mut [u8]) -> Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        poll_fn(|cx| {
            // Fast path: try a non-blocking read first.
            match self.inner.read(buf, Duration::ZERO) {
                Ok(n) if n > 0 => return Poll::Ready(Ok(n)),
                Ok(_) | Err(Error::WouldBlock) | Err(Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            // Register + recheck.
            self.waker.register(cx.waker());
            match self.inner.read(buf, Duration::ZERO) {
                Ok(n) if n > 0 => Poll::Ready(Ok(n)),
                Ok(_) | Err(Error::WouldBlock) | Err(Error::Timeout) => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    /// Borrow the underlying [`Uart`] for synchronous operations
    /// (`write`, `bytes_available`, `flush`).
    #[inline]
    pub fn inner(&self) -> &Uart {
        &self.inner
    }
}

unsafe extern "C" fn uart_notify_trampoline(user_data: *mut core::ffi::c_void) {
    let waker = unsafe { &*(user_data as *const AtomicWaker) };
    waker.wake();
}
