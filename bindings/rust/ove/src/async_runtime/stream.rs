// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async wrapper around [`crate::Stream`] using the C-level
//! `ove_stream_set_notify` notify hook.
//!
//! The pattern is the canonical "embassy primitive on top of an RTOS
//! synchronous primitive":
//!
//!   1. The wrapper owns an `AtomicWaker`.
//!   2. On construction it registers a C callback against the underlying
//!      stream; the callback is `AtomicWaker::wake` via a void* user_data.
//!   3. `recv_async` polls the non-blocking `Stream::try_recv` first,
//!      and on `WouldBlock` registers `cx.waker()` into the AtomicWaker,
//!      then re-checks (avoids lost-wake races against the producer).
//!
//! Lifetime constraint: the C callback retains a pointer to the
//! AtomicWaker, so the AsyncStream's address must be stable for its
//! entire lifetime. The simplest pattern is a `static` declaration or
//! `Box::leak`. Methods take `&'static self` to enforce this at the
//! type level.

use core::future::poll_fn;
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::error::{Error, Result};
use crate::stream::Stream;

/// Async wrapper around an [`ove::Stream`](crate::Stream).
///
/// Owns the wrapped stream and the `AtomicWaker` used to bridge the
/// C-level notify hook into the embassy executor's wake path.
pub struct AsyncStream<const N: usize> {
    inner: Stream<N>,
    waker: AtomicWaker,
}

// SAFETY: `Stream<N>` is itself Send+Sync (handle is a kernel-managed
// pointer); `AtomicWaker` is Sync. The wrapper inherits both.
unsafe impl<const N: usize> Send for AsyncStream<N> {}
unsafe impl<const N: usize> Sync for AsyncStream<N> {}

impl<const N: usize> AsyncStream<N> {
    /// Wrap a stream for async use. Does not arm the notify hook yet —
    /// call [`Self::arm`] after the wrapper has reached its final
    /// 'static location (the C callback retains a pointer to the
    /// internal `AtomicWaker`, so the wrapper must not move after
    /// arming).
    pub const fn new(inner: Stream<N>) -> Self {
        Self {
            inner,
            waker: AtomicWaker::new(),
        }
    }

    /// Register the C-side notify callback. Must be called exactly once,
    /// and only after the wrapper is at its final 'static address.
    pub fn arm(&'static self) -> Result<()> {
        // SAFETY: the wrapper is 'static, so the waker pointer is valid
        // for the rest of the program's lifetime; `notify_trampoline`
        // dereferences it as `&AtomicWaker`, which is Sync.
        unsafe {
            self.inner.set_notify(
                Some(notify_trampoline),
                &self.waker as *const AtomicWaker as *mut core::ffi::c_void,
            )
        }
    }

    /// Receive up to `buf.len()` bytes. Awaits until at least one byte
    /// is available, then returns the number of bytes copied.
    ///
    /// Returns `Ok(0)` only if `buf.is_empty()`.
    pub async fn read(&'static self, buf: &mut [u8]) -> Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        poll_fn(|cx| {
            // Fast path — try non-blocking read before touching the
            // waker. Covers the common case where bytes are already
            // pending.
            match self.inner.try_recv(buf) {
                Ok(n) if n > 0 => return Poll::Ready(Ok(n)),
                Ok(_) | Err(Error::WouldBlock) | Err(Error::Timeout) => {}
                Err(e) => return Poll::Ready(Err(e)),
            }
            // Register, then re-check. The recheck closes the race where
            // a producer fires the notify callback (which wakes the
            // current waker, not ours) between our first try_recv and
            // the register call.
            self.waker.register(cx.waker());
            match self.inner.try_recv(buf) {
                Ok(n) if n > 0 => Poll::Ready(Ok(n)),
                Ok(_) | Err(Error::WouldBlock) | Err(Error::Timeout) => Poll::Pending,
                Err(e) => Poll::Ready(Err(e)),
            }
        })
        .await
    }

    /// Borrow the underlying [`Stream`] for synchronous operations
    /// (`send`, `bytes_available`, …). The async `read` is the only
    /// path that touches the AtomicWaker; non-blocking access through
    /// this borrow is independent.
    #[inline]
    pub fn inner(&self) -> &Stream<N> {
        &self.inner
    }
}

/// C-ABI trampoline invoked by `ove_stream_send` / `_send_from_isr`
/// after a successful write. `user_data` is the address of the
/// AtomicWaker inside an `AsyncStream` (pinned to 'static by the
/// `arm()` contract).
unsafe extern "C" fn notify_trampoline(user_data: *mut core::ffi::c_void) {
    // SAFETY: the pointer was set up by AsyncStream::arm; the target is
    // a valid AtomicWaker pinned for 'static. AtomicWaker::wake is
    // lock-free and ISR-safe.
    let waker = unsafe { &*(user_data as *const AtomicWaker) };
    waker.wake();
}
