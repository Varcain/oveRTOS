// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Work queue and deferred work items for oveRTOS.
//!
//! A [`Workqueue`] runs a dedicated thread that processes [`Work`] items in order.
//! Work items can be submitted immediately or after a delay, and can be cancelled
//! before they execute.

use crate::bindings;
use crate::error::{Error, Result};

/// A workqueue that executes deferred work items on a dedicated thread.
pub struct Workqueue {
    handle: bindings::ove_workqueue_t,
}

impl Workqueue {
    /// Create a workqueue via heap allocation (only in heap mode).
    ///
    /// `name` must be a `&'static CStr`; the easiest source is a
    /// `c"..."` literal (Rust 1.77+).  The compiler enforces null
    /// termination — `&[u8]` without `\0` was the previous shape and
    /// was undefined-behaviour-prone.
    #[cfg(not(zero_heap))]
    pub fn new(
        name: &'static core::ffi::CStr,
        priority: crate::Priority,
        stack_size: usize,
    ) -> Result<Self> {
        let mut handle: bindings::ove_workqueue_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_workqueue_create(
                &mut handle,
                name.as_ptr(),
                priority as bindings::ove_prio_t,
                stack_size,
            )
        };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage and stack.
    ///
    /// # Safety
    /// - `storage` must outlive the `Workqueue` and not be shared.
    /// - `stack` must point to at least `stack_size` bytes and outlive
    ///   the `Workqueue`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_workqueue_storage_t,
        name: &'static core::ffi::CStr,
        priority: crate::Priority,
        stack_size: usize,
        stack: *mut core::ffi::c_void,
    ) -> Result<Self> {
        let mut handle: bindings::ove_workqueue_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_workqueue_init(
                &mut handle,
                storage,
                name.as_ptr(),
                priority as bindings::ove_prio_t,
                stack_size,
                stack,
            )
        };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Return the underlying `ove_workqueue_t` handle for advanced
    /// FFI interop.  Normal app code should pass the [`Workqueue`]
    /// itself to [`Work::submit`] / [`Work::submit_delayed`].
    pub fn handle(&self) -> bindings::ove_workqueue_t {
        self.handle
    }
}

crate::ove_handle_impl!(Workqueue, ove_workqueue_destroy, ove_workqueue_deinit);

/// A work item that can be submitted to a [`Workqueue`].
pub struct Work {
    handle: bindings::ove_work_t,
}

impl Work {
    /// Create a work item via heap allocation (only in heap mode).
    ///
    /// The handler receives the raw `ove_work_t` handle (the C API does
    /// not provide a user_data parameter for work handlers).
    ///
    /// # Errors
    /// Returns [`Error::NoMemory`] if heap allocation fails.
    #[cfg(not(zero_heap))]
    pub fn new(handler: bindings::ove_work_fn) -> Result<Self> {
        let mut handle: bindings::ove_work_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_work_init(&mut handle, handler) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Work` item.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_work_storage_t,
        handler: bindings::ove_work_fn,
    ) -> Result<Self> {
        let mut handle: bindings::ove_work_t = core::ptr::null_mut();
        let rc = unsafe { bindings::ove_work_init_static(&mut handle, storage, handler) };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Submit this work item to `wq` for immediate execution.
    ///
    /// # Errors
    /// Returns an error if the workqueue is shutting down or the item is already pending.
    pub fn submit(&self, wq: &Workqueue) -> Result<()> {
        let rc = unsafe { bindings::ove_work_submit(wq.handle(), self.handle) };
        Error::from_code(rc)
    }

    /// Submit this work item to `wq` for execution after `delay_ms` milliseconds.
    ///
    /// # Errors
    /// Returns an error if the workqueue is shutting down or the item is already pending.
    pub fn submit_delayed(&self, wq: &Workqueue, delay_ms: u32) -> Result<()> {
        let rc = unsafe { bindings::ove_work_submit_delayed(wq.handle(), self.handle, delay_ms) };
        Error::from_code(rc)
    }

    /// Cancel this work item if it is pending or delayed.
    ///
    /// Has no effect if the item is not currently queued.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    pub fn cancel(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_work_cancel(self.handle) };
        Error::from_code(rc)
    }
}

impl Drop for Work {
    fn drop(&mut self) {
        if self.handle.is_null() {
            return;
        }
        #[cfg(not(zero_heap))]
        unsafe {
            bindings::ove_work_free(self.handle);
        }
        #[cfg(zero_heap)]
        {
            self.handle = core::ptr::null_mut();
        }
    }
}

unsafe impl Send for Work {}
unsafe impl Sync for Work {}
