// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Software timer for oveRTOS.
//!
//! [`Timer`] wraps an RTOS software timer with a safe Rust `fn()` callback.
//! Timers can be periodic or one-shot and are driven by the RTOS tick.

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

/// Caller-owned storage for a [`Timer`] in zero-heap mode (see
/// [`crate::MutexStorage`]).
#[allow(dead_code)]
pub struct TimerStorage {
    storage: UnsafeCell<MaybeUninit<bindings::ove_timer_storage_t>>,
}

impl TimerStorage {
    /// Zero-initialised storage.  `const` so it can initialise a `static`.
    #[inline]
    pub const fn new() -> Self {
        Self {
            storage: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

impl Default for TimerStorage {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: see crate::MutexStorage.
unsafe impl Sync for TimerStorage {}

/// Software timer with a safe Rust callback.
///
/// The timer stores a function pointer and generates an internal trampoline
/// so the user callback is a plain `fn()` — no `unsafe`, no raw pointers.
pub struct Timer {
    handle: bindings::ove_timer_t,
}

impl Timer {
    /// Create a new timer via heap allocation (only in heap mode).
    ///
    /// - `callback` — safe Rust function called each time the timer fires.
    /// - `period_ms` — timer period in milliseconds.
    /// - `one_shot` — if `true`, the timer fires once and stops.
    #[cfg(not(zero_heap))]
    pub fn new(callback: fn(), period_ms: u32, one_shot: bool) -> Result<Self> {
        // Store the callback as the user_data pointer. On platforms where
        // fn() is pointer-sized this is a direct cast; the trampoline
        // reconverts it.
        let user_data = callback as *mut core::ffi::c_void;

        let mut handle: bindings::ove_timer_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_timer_create(
                &mut handle,
                Some(Self::trampoline),
                user_data,
                period_ms,
                one_shot as i32,
            )
        };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Create from caller-provided static storage.
    ///
    /// # Safety
    /// Caller must ensure `storage` outlives the `Timer`.
    #[cfg(zero_heap)]
    pub unsafe fn from_static(
        storage: *mut bindings::ove_timer_storage_t,
        callback: fn(),
        period_ms: u32,
        one_shot: bool,
    ) -> Result<Self> {
        let user_data = callback as *mut core::ffi::c_void;
        let mut handle: bindings::ove_timer_t = core::ptr::null_mut();
        let rc = unsafe {
            bindings::ove_timer_init(
                &mut handle,
                storage,
                Some(Self::trampoline),
                user_data,
                period_ms,
                one_shot as i32,
            )
        };
        Error::from_code(rc)?;
        Ok(Self { handle })
    }

    /// Mode-agnostic constructor (see [`crate::Mutex::create`]).  Heap mode
    /// ignores `storage`; zero-heap mode backs the timer with it.
    pub fn create(
        storage: &'static TimerStorage,
        callback: fn(),
        period_ms: u32,
        one_shot: bool,
    ) -> Result<Self> {
        #[cfg(not(zero_heap))]
        {
            let _ = storage;
            Self::new(callback, period_ms, one_shot)
        }
        #[cfg(zero_heap)]
        {
            let ptr = UnsafeCell::raw_get(&storage.storage).cast();
            unsafe { Self::from_static(ptr, callback, period_ms, one_shot) }
        }
    }

    /// Start the timer, beginning countdown from now.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    #[inline]
    pub fn start(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_timer_start(self.handle) };
        Error::from_code(rc)
    }

    /// Stop the timer, preventing further callbacks until restarted.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    #[inline]
    pub fn stop(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_timer_stop(self.handle) };
        Error::from_code(rc)
    }

    /// Reset the timer, restarting the period from now.
    ///
    /// If the timer is stopped, this also starts it.
    ///
    /// # Errors
    /// Returns an error if the underlying RTOS call fails.
    #[inline]
    pub fn reset(&self) -> Result<()> {
        let rc = unsafe { bindings::ove_timer_reset(self.handle) };
        Error::from_code(rc)
    }

    /// Internal trampoline that converts the C callback into a safe Rust call.
    unsafe extern "C" fn trampoline(
        _timer: bindings::ove_timer_t,
        user_data: *mut core::ffi::c_void,
    ) {
        // SAFETY: `user_data` was stored by `Timer::new`/`from_static` from a
        // `fn()` pointer. Targets supported by oveRTOS have pointer-sized
        // function pointers with a C-compatible ABI, so round-tripping
        // through `*mut c_void` is well-defined.
        let cb: fn() = unsafe { core::mem::transmute(user_data) };
        cb();
    }
}

crate::ove_handle_impl!(Timer, ove_timer_destroy, ove_timer_deinit);
