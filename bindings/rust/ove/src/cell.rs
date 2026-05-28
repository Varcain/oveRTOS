// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Single-threaded interior-mutability primitives.
//!
//! [`LvCell<T>`] and [`LvRefCell<T>`] are thin newtypes around
//! `core::cell::Cell` / `RefCell` that are `Sync`. The name carries a
//! *contract*, not a runtime enforcement: the cell may only be mutated
//! from one thread at a time. In LVGL apps the global LVGL lock enforces
//! this; in other contexts, the caller must ensure single-threaded
//! access (e.g. the cell is only touched inside a benchmark's
//! setup/run/teardown on one thread at a time).

/// `Cell<T>` for state mutated only from a single logical thread at a time.
/// Marked `Sync` — the caller is responsible for upholding the
/// single-access invariant (e.g. via a lock like LVGL's, or by structural
/// guarantees in an embedded setup/run/teardown flow).
///
/// Pairs naturally with [`crate::InitCell`] for app-wide state.
#[repr(transparent)]
pub struct LvCell<T>(core::cell::Cell<T>);

// SAFETY: The caller promises single-threaded access at any given time.
// `T: Send` is required because observations may cross threads as long
// as they don't overlap in time.
unsafe impl<T: Send> Sync for LvCell<T> {}

impl<T> LvCell<T> {
    /// Construct a new cell holding `v`. Usable in `static`/`const` contexts.
    pub const fn new(v: T) -> Self {
        Self(core::cell::Cell::new(v))
    }

    /// Replace the value, returning the previous one.
    pub fn replace(&self, v: T) -> T {
        self.0.replace(v)
    }

    /// Store `v`.
    pub fn set(&self, v: T) {
        self.0.set(v);
    }

    /// Borrow the contents without copying.
    ///
    /// # Safety contract
    ///
    /// Same single-threaded-access invariant as the rest of `LvCell`:
    /// the caller must ensure no concurrent mutation while the
    /// returned reference is live.  Useful for hot loops over large
    /// `Copy` payloads (e.g. multi-byte buffers in benchmark inner
    /// loops) where `get()`'s implicit copy would dominate.
    #[inline]
    pub fn get_ref(&self) -> &T {
        // SAFETY: `LvCell` already promises single-threaded access at
        // a time; readers and writers do not overlap.  Cell::as_ptr
        // returns the same address for the entire life of the cell,
        // so the returned reference is valid for the cell's lifetime
        // up to the next mutation.
        unsafe { &*self.0.as_ptr() }
    }

    /// Raw pointer to the contents — same caveat as
    /// [`core::cell::Cell::as_ptr`].  Combined with the LvCell
    /// single-threaded-access invariant this lets a hot path build
    /// a temporary `&mut T` for in-place updates without going
    /// through `get()`'s Copy + `set()`'s Copy round-trip.
    #[inline]
    pub fn as_ptr(&self) -> *mut T {
        self.0.as_ptr()
    }
}

impl<T: Copy> LvCell<T> {
    /// Read the current value (copies).
    pub fn get(&self) -> T {
        self.0.get()
    }

    /// Apply `f` to the current value and store the result.
    pub fn update(&self, f: impl FnOnce(T) -> T) {
        let v = self.0.get();
        self.0.set(f(v));
    }
}

/// `RefCell<T>` variant of [`LvCell`] for non-`Copy` state.
#[repr(transparent)]
pub struct LvRefCell<T>(core::cell::RefCell<T>);

// SAFETY: `LvRefCell<T>` is a `RefCell`-shaped interior-mutable cell whose
// borrow tracking is sound only while the LVGL lock is held.  `T: Send` is
// sufficient because all access happens inside the LVGL task or behind
// `LvglGuard`, both of which serialise readers and the single writer.
unsafe impl<T: Send> Sync for LvRefCell<T> {}

impl<T> LvRefCell<T> {
    /// Construct a new ref-cell. Usable in `static`/`const` contexts.
    pub const fn new(v: T) -> Self {
        Self(core::cell::RefCell::new(v))
    }

    /// Borrow the contents immutably (panics on outstanding mutable borrow).
    pub fn borrow(&self) -> core::cell::Ref<'_, T> {
        self.0.borrow()
    }

    /// Borrow the contents mutably (panics on outstanding borrow).
    pub fn borrow_mut(&self) -> core::cell::RefMut<'_, T> {
        self.0.borrow_mut()
    }

    /// Apply `f` to a mutable borrow.
    pub fn with_mut<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        f(&mut *self.0.borrow_mut())
    }
}
