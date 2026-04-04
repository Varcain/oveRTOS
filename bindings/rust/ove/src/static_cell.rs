// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Init-once container for `static` declarations.
//!
//! `StaticCell<T>` encapsulates the single `unsafe` pattern of "create once in
//! `on_init`, use from any thread, destroy in `on_shutdown`" behind a safe API.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

/// A `no_std` init-once container designed for the oveRTOS lifecycle.
///
/// - `const fn new()` — usable in `static` declarations
/// - `init()` — set value (panics if already set)
/// - `get()` — get reference (panics if not set)
/// - `try_get()` — non-panicking variant
/// - `shutdown()` — drop contained value, mark uninitialized
///
/// # Safety contract
///
/// `init()` and `shutdown()` must be called single-threaded (framework lifecycle
/// guarantee: `on_init` / `on_shutdown`). Between init and shutdown the value is
/// immutable — no data race is possible.
pub struct StaticCell<T> {
    initialized: AtomicBool,
    inner: UnsafeCell<Option<T>>,
}

impl<T> StaticCell<T> {
    /// Create an empty cell. Usable in `static` declarations.
    pub const fn new() -> Self {
        Self {
            initialized: AtomicBool::new(false),
            inner: UnsafeCell::new(None),
        }
    }

    /// Initialize the cell with `val`.
    ///
    /// # Panics
    /// Panics if the cell is already initialized.
    ///
    /// # Safety contract
    /// Must be called single-threaded (e.g. in `on_init`).
    pub fn init(&self, val: T) {
        if self.initialized.load(Ordering::Relaxed) {
            panic!("StaticCell::init called on already-initialized cell");
        }
        // SAFETY: Single-threaded during init (lifecycle guarantee).
        unsafe {
            *self.inner.get() = Some(val);
        }
        self.initialized.store(true, Ordering::Release);
    }

    /// Get a reference to the contained value.
    ///
    /// # Panics
    /// Panics if the cell has not been initialized.
    pub fn get(&self) -> &T {
        if !self.initialized.load(Ordering::Acquire) {
            panic!("StaticCell::get called on uninitialized cell");
        }
        // SAFETY: After init (Release) + this Acquire, the value is immutable
        // until shutdown (which is single-threaded). No data race.
        unsafe { (*self.inner.get()).as_ref().unwrap() }
    }

    /// Try to initialize the cell. Returns `Err(val)` if already initialized,
    /// so the caller doesn't lose the value.
    ///
    /// # Safety contract
    /// Same as `init()` — must be called single-threaded.
    pub fn try_init(&self, val: T) -> Result<(), T> {
        if self.initialized.load(Ordering::Relaxed) {
            return Err(val);
        }
        // SAFETY: Single-threaded during init (lifecycle guarantee).
        unsafe {
            *self.inner.get() = Some(val);
        }
        self.initialized.store(true, Ordering::Release);
        Ok(())
    }

    /// Try to get a reference, returning `None` if not initialized.
    pub fn try_get(&self) -> Option<&T> {
        if !self.initialized.load(Ordering::Acquire) {
            return None;
        }
        // SAFETY: Same argument as `get()`.
        unsafe { (*self.inner.get()).as_ref() }
    }

    /// Drop the contained value and mark the cell as uninitialized.
    ///
    /// Idempotent — no-op if already empty.
    ///
    /// # Safety contract
    /// Must be called single-threaded (e.g. in `on_shutdown`).
    pub fn shutdown(&self) {
        if self.initialized.load(Ordering::Relaxed) {
            self.initialized.store(false, Ordering::Release);
            // SAFETY: Single-threaded during shutdown (lifecycle guarantee).
            // No other thread can observe the value after initialized=false.
            unsafe {
                *self.inner.get() = None;
            }
        }
    }
}

impl<T> core::ops::Deref for StaticCell<T> {
    type Target = T;
    fn deref(&self) -> &T { self.get() }
}

// SAFETY: The init/shutdown lifecycle is single-threaded. Between those points
// the value is immutable (shared &T only). The AtomicBool + Release/Acquire
// ordering ensures cross-thread visibility.
unsafe impl<T: Send + Sync> Sync for StaticCell<T> {}
unsafe impl<T: Send> Send for StaticCell<T> {}

/// Like `StaticCell` but provides `&mut T` access through `UnsafeCell`.
///
/// Used for types that need mutable access from a single owner thread
/// (e.g., DspEngine from audio ISR, IrManager from loader thread).
///
/// # Safety contract
///
/// - `init()` and `shutdown()` must be called single-threaded (lifecycle guarantee).
/// - `get_mut()` requires the caller to ensure exclusive access.
pub struct StaticMut<T> {
    initialized: AtomicBool,
    inner: UnsafeCell<Option<T>>,
}

impl<T> StaticMut<T> {
    /// Create an empty cell. Usable in `static` declarations.
    pub const fn new() -> Self {
        Self {
            initialized: AtomicBool::new(false),
            inner: UnsafeCell::new(None),
        }
    }

    /// Initialize the cell with `val`.
    ///
    /// # Panics
    /// Panics if the cell is already initialized.
    pub fn init(&self, val: T) {
        if self.initialized.load(Ordering::Relaxed) {
            panic!("StaticMut::init called on already-initialized cell");
        }
        unsafe { *self.inner.get() = Some(val) };
        self.initialized.store(true, Ordering::Release);
    }

    /// Try to initialize the cell. Returns `Err(val)` if already initialized,
    /// so the caller doesn't lose the value.
    ///
    /// # Safety contract
    /// Same as `init()` — must be called single-threaded.
    pub fn try_init(&self, val: T) -> Result<(), T> {
        if self.initialized.load(Ordering::Relaxed) {
            return Err(val);
        }
        unsafe { *self.inner.get() = Some(val) };
        self.initialized.store(true, Ordering::Release);
        Ok(())
    }

    /// Get a mutable reference to the contained value.
    ///
    /// # Safety
    /// Caller must ensure exclusive access (single-threaded or external synchronization).
    pub unsafe fn get_mut(&self) -> &mut T {
        debug_assert!(self.initialized.load(Ordering::Acquire));
        unsafe { (*self.inner.get()).as_mut().unwrap() }
    }

    /// Try to get an immutable reference, returning `None` if not initialized.
    pub fn try_get(&self) -> Option<&T> {
        if !self.initialized.load(Ordering::Acquire) {
            return None;
        }
        unsafe { (*self.inner.get()).as_ref() }
    }

    /// Get an immutable reference to the contained value.
    ///
    /// # Panics
    /// Panics if the cell has not been initialized.
    pub fn get(&self) -> &T {
        if !self.initialized.load(Ordering::Acquire) {
            panic!("StaticMut::get called on uninitialized cell");
        }
        unsafe { (*self.inner.get()).as_ref().unwrap() }
    }

    /// Drop the contained value and mark the cell as uninitialized.
    ///
    /// Idempotent — no-op if already empty.
    pub fn shutdown(&self) {
        if self.initialized.load(Ordering::Relaxed) {
            self.initialized.store(false, Ordering::Release);
            unsafe { *self.inner.get() = None };
        }
    }
}

impl<T> core::ops::Deref for StaticMut<T> {
    type Target = T;
    fn deref(&self) -> &T { self.get() }
}

// SAFETY: StaticMut provides `&mut T` access only through an unsafe method
// that requires the caller to prove exclusive access. Init/shutdown are
// single-threaded (lifecycle guarantee). `T: Sync` is required because
// shared references (`&T` via `Deref`) may be accessed from multiple threads.
unsafe impl<T: Send + Sync> Sync for StaticMut<T> {}
unsafe impl<T: Send> Send for StaticMut<T> {}
