// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Byte-stream buffer for oveRTOS.
//!
//! [`Stream<N>`] is a lock-free ring buffer for passing arbitrary byte sequences
//! between threads or between ISR and thread contexts. Unlike [`crate::Queue`], which
//! transfers discrete items, a `Stream` treats data as a continuous byte flow with
//! a configurable receive-trigger threshold.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;

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

/// Caller-owned storage + byte buffer for a [`Stream`] in zero-heap mode.
/// Declare in a `static` and pass `&STORAGE` to [`Stream::create`]; in heap
/// mode it is ignored.  See [`crate::MutexStorage`]; this one additionally
/// embeds the `[u8; N]` ring buffer the stream needs.
#[allow(dead_code)]
pub struct StreamStorage<const N: usize> {
    storage: UnsafeCell<MaybeUninit<bindings::ove_stream_storage_t>>,
    buffer: UnsafeCell<MaybeUninit<[u8; N]>>,
}

impl<const N: usize> StreamStorage<N> {
    /// Zero-initialised storage with an uninitialised byte buffer.  `const`
    /// so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
            buffer: UnsafeCell::new(MaybeUninit::uninit()),
        }
    }
}

impl<const N: usize> Default for StreamStorage<N> {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see crate::MutexStorage.
unsafe impl<const N: usize> Sync for StreamStorage<N> {}

/// Byte-oriented stream buffer with compile-time buffer size.
///
/// Wraps `ove_stream_t` for passing variable-length byte data between
/// threads or between ISR and thread contexts. `N` is the buffer size in bytes.
pub struct Stream<const N: usize> {
    handle: bindings::ove_stream_t,
}

impl<const N: usize> Stream<N> {
    /// Create a stream via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new(trigger: usize) -> Result<Self> {
        let mut handle: bindings::ove_stream_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_stream_create(&mut handle, N, trigger) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage and buffer.
    ///
    /// # Safety
    /// - `storage` must outlive the `Stream` and not be shared.
    /// - `buffer` must point to at least `N` bytes and outlive the `Stream`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_stream_storage_t,
        buffer: *mut core::ffi::c_void,
        trigger: usize,
    ) -> Result<Self> {
        let mut handle: bindings::ove_stream_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_stream_init(&mut handle, storage, buffer, N, trigger) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Mode-agnostic constructor (see [`crate::Mutex::create`]).  Heap mode
    /// ignores `storage`; zero-heap mode backs the stream with its storage and
    /// embedded byte buffer.
    pub fn create(storage: &'static StreamStorage<N>, trigger: usize) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new(trigger)
        }
        #[cfg(zero_heap)]
        {
            let sptr = UnsafeCell::raw_get(&storage.storage).cast();
            let bptr = UnsafeCell::raw_get(&storage.buffer).cast::<core::ffi::c_void>();
            unsafe { Self::from_static(sptr, bptr, trigger) }
        }
    }

    /// Send bytes, blocking indefinitely if the buffer is full.
    /// Returns the number of bytes actually sent (may be < `data.len()`).
    #[inline]
    pub fn send(&self, data: &[u8]) -> Result<usize> {
        self.send_with_timeout(data, u64::MAX)
    }

    /// Non-blocking send.  Returns the number of bytes actually sent.
    ///
    /// # Errors
    /// Returns [`Error::WouldBlock`] if the buffer is full and no
    /// bytes could be written.
    #[inline]
    pub fn try_send(&self, data: &[u8]) -> Result<usize> {
        self.send_with_timeout(data, 0)
    }

    /// Send up to `d`.  Returns the number of bytes actually sent.
    #[inline]
    pub fn try_send_for(&self, data: &[u8], d: core::time::Duration) -> Result<usize> {
        self.send_with_timeout(data, crate::time::dur_to_ns(d))
    }

    /// Send by the given deadline.  Returns the number of bytes
    /// actually sent.
    #[inline]
    pub fn try_send_until(&self, data: &[u8], deadline: crate::time::Instant) -> Result<usize> {
        self.send_with_timeout(data, crate::time::deadline_to_timeout_ns(deadline))
    }

    #[inline]
    fn send_with_timeout(&self, data: &[u8], timeout_ns: u64) -> Result<usize> {
        let mut bytes_sent: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_send(
                self.handle,
                data.as_ptr() as *const _,
                data.len(),
                timeout_ns,
                &mut bytes_sent,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_sent)
    }

    /// Receive bytes, blocking indefinitely.  Returns the number of
    /// bytes actually read (may be < `buf.len()` — blocks until at
    /// least the trigger byte count is available).
    #[inline]
    pub fn recv(&self, buf: &mut [u8]) -> Result<usize> {
        self.recv_with_timeout(buf, u64::MAX)
    }

    /// Non-blocking receive.  Returns the number of bytes read.
    ///
    /// # Errors
    /// Returns [`Error::WouldBlock`] if no bytes are available.
    #[inline]
    pub fn try_recv(&self, buf: &mut [u8]) -> Result<usize> {
        self.recv_with_timeout(buf, 0)
    }

    /// Receive up to `d`.
    #[inline]
    pub fn try_recv_for(&self, buf: &mut [u8], d: core::time::Duration) -> Result<usize> {
        self.recv_with_timeout(buf, crate::time::dur_to_ns(d))
    }

    /// Receive by the given deadline.
    #[inline]
    pub fn try_recv_until(&self, buf: &mut [u8], deadline: crate::time::Instant) -> Result<usize> {
        self.recv_with_timeout(buf, crate::time::deadline_to_timeout_ns(deadline))
    }

    #[inline]
    fn recv_with_timeout(&self, buf: &mut [u8], timeout_ns: u64) -> Result<usize> {
        let mut bytes_received: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_receive(
                self.handle,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                timeout_ns,
                &mut bytes_received,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_received)
    }

    /// Send bytes from an ISR context (non-blocking, returns immediately).
    ///
    /// Returns the number of bytes sent; may be less than `data.len()` if the buffer fills.
    ///
    /// # Errors
    /// Returns an error if the stream is full.
    #[inline]
    pub fn send_from_isr(&self, data: &[u8]) -> Result<usize> {
        let mut bytes_sent: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_send_from_isr(
                self.handle,
                data.as_ptr() as *const _,
                data.len(),
                &mut bytes_sent,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_sent)
    }

    /// Receive bytes from an ISR context (non-blocking, returns immediately).
    ///
    /// Returns the number of bytes received; may be zero if no data is available.
    ///
    /// # Errors
    /// Returns an error if the stream is empty.
    #[inline]
    pub fn receive_from_isr(&self, buf: &mut [u8]) -> Result<usize> {
        let mut bytes_received: usize = 0;
        let rc = unsafe {
            bindings::ove_stream_receive_from_isr(
                self.handle,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                &mut bytes_received,
            )
        };
        Error::from_code(rc)?;
        Ok(bytes_received)
    }

    /// Reset the stream, discarding all currently buffered data.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    #[inline]
    pub fn reset(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_stream_reset(self.handle) };
        Error::from_code(rc)
    }

    /// Return the number of bytes currently available for reading.
    #[inline]
    pub fn bytes_available(&self) -> usize {
        unsafe { bindings::ove_stream_bytes_available(self.handle) }
    }

    /// Register a notify callback fired after every successful send.
    /// Wraps the C-level `ove_stream_set_notify`.
    ///
    /// # Safety
    /// - `user_data` must remain valid for as long as the callback may
    ///   fire — i.e. until either the stream is destroyed or
    ///   `set_notify(None, ...)` clears the registration.
    /// - `cb` may be invoked from any context the producer uses,
    ///   including ISR. Its body must therefore be non-blocking and
    ///   ISR-safe.
    ///
    /// Higher-level users should reach for the async wrappers in
    /// `ove::async_runtime` instead of using this directly — they hide
    /// the lifetime and ISR-safety constraints behind a safe API.
    #[cfg(has_async)]
    #[inline]
    pub unsafe fn set_notify(
        &self,
        cb: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
        user_data: *mut core::ffi::c_void,
    ) -> Result<()> {
        let rc = unsafe { bindings::ove_stream_set_notify(self.handle, cb, user_data) };
        Error::from_code(rc)
    }
}

crate::ove_handle_impl!(Stream<const N: usize>, ove_stream_destroy, ove_stream_deinit);
