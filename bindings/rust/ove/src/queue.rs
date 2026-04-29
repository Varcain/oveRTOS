// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Type-safe FIFO queue for oveRTOS.
//!
//! [`Queue<T, N>`] is a fixed-capacity, thread-safe FIFO that transfers items of
//! type `T` (which must be `Copy`) between threads or between ISR and thread
//! contexts. `N` is the maximum number of items the queue can hold.

use core::fmt;
use core::marker::PhantomData;
use core::mem;

use crate::bindings;
use crate::error::{Error, Result};

/// Type-safe FIFO queue with compile-time capacity.
///
/// `T` must be a plain-old-data type (Copy) because items are transferred
/// through the C API via raw byte copies. `N` is the queue capacity.
pub struct Queue<T: Copy, const N: usize> {
    handle: bindings::ove_queue_t,
    _marker: PhantomData<T>,
}

impl<T: Copy, const N: usize> Queue<T, N> {
    /// Create a queue via heap allocation (only in heap mode).
    #[cfg(not(zero_heap))]
    pub fn new() -> Result<Self> {
        let mut handle: bindings::ove_queue_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_queue_create(&mut handle, mem::size_of::<T>(), N as u32) };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            _marker: PhantomData,
        })
    }

    /// Create from caller-provided static storage and buffer.
    ///
    /// # Safety
    /// - `storage` must outlive the `Queue` and not be shared.
    /// - `buffer` must point to at least `N * size_of::<T>()` bytes
    ///   and outlive the `Queue`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_queue_storage_t,
        buffer: *mut core::ffi::c_void,
    ) -> Result<Self> {
        let mut handle: bindings::ove_queue_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_queue_init(&mut handle, storage, buffer, mem::size_of::<T>(), N as u32)
        };
        Error::from_code(rc)?;
        Ok(Self {
            handle,
            _marker: PhantomData,
        })
    }

    /// Send an item to the queue, blocking up to `timeout_ms` if the queue is full.
    ///
    /// # Errors
    /// Returns [`Error::QueueFull`] or [`Error::Timeout`] if the item cannot be
    /// enqueued within `timeout_ms`.
    #[inline]
    pub fn send(&self, item: &T, timeout_ms: u32) -> Result<()> {
        let rc = unsafe {
            bindings::ove_queue_send(self.handle, item as *const T as *const _, timeout_ms)
        };
        Error::from_code(rc)
    }

    /// Receive an item from the queue, blocking up to `timeout_ms` if the queue is empty.
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if no item is available within `timeout_ms`.
    #[inline]
    pub fn receive(&self, timeout_ms: u32) -> Result<T> {
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc = unsafe {
            bindings::ove_queue_receive(self.handle, item.as_mut_ptr() as *mut _, timeout_ms)
        };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }

    /// Send an item from an ISR context (non-blocking, returns immediately if full).
    ///
    /// # Errors
    /// Returns [`Error::QueueFull`] if the queue has no space.
    pub fn send_from_isr(&self, item: &T) -> Result<()> {
        let rc =
            unsafe { bindings::ove_queue_send_from_isr(self.handle, item as *const T as *const _) };
        Error::from_code(rc)
    }

    /// Receive an item from an ISR context (non-blocking, returns immediately if empty).
    ///
    /// # Errors
    /// Returns [`Error::Timeout`] if the queue is empty.
    pub fn receive_from_isr(&self) -> Result<T> {
        let mut item: mem::MaybeUninit<T> = mem::MaybeUninit::uninit();
        let rc = unsafe {
            bindings::ove_queue_receive_from_isr(self.handle, item.as_mut_ptr() as *mut _)
        };
        Error::from_code(rc)?;
        Ok(unsafe { item.assume_init() })
    }
}

impl<T: Copy, const N: usize> fmt::Debug for Queue<T, N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Queue")
            .field("handle", &format_args!("{:p}", self.handle))
            .finish()
    }
}

impl<T: Copy, const N: usize> Drop for Queue<T, N> {
    fn drop(&mut self) {
        if self.handle.is_null() {
            return;
        }
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_queue_destroy(self.handle);
        }
        #[cfg(zero_heap)]
        unsafe {
            bindings::ove_queue_deinit(self.handle);
        }
    }
}

unsafe impl<T: Copy + Send, const N: usize> Send for Queue<T, N> {}
unsafe impl<T: Copy + Send, const N: usize> Sync for Queue<T, N> {}
