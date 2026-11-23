// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! User trace marks and zones. With trace markers disabled, marks and zones
//! are no-ops and [`name`] returns `false`.
//!
//! Marks and zones use the calling thread's lane or the interrupt lane.

#[cfg(has_trace)]
use crate::bindings;
use core::ffi::CStr;
use core::marker::PhantomData;
#[cfg(has_trace)]
use core::sync::atomic::{AtomicBool, Ordering};

/// Append a name for an id from a single registering thread.
///
/// Copies up to 31 bytes into one of 32 entries. Re-registering an id appends
/// an entry, and the latest entry wins. Returns `false` for an empty name,
/// a full table, disabled markers, or an overlapping Rust registration.
#[inline]
#[must_use]
pub fn name(id: u16, name: &CStr) -> bool {
    #[cfg(has_trace)]
    {
        static REGISTERING: AtomicBool = AtomicBool::new(false);
        if REGISTERING.swap(true, Ordering::Acquire) {
            return false;
        }
        // SAFETY: The guard excludes other safe Rust writers, and CStr is
        // NUL-terminated for the duration of the C call, which copies it.
        // C and C++ registrations bypass the guard and must obey the single-writer rule.
        let registered = unsafe { bindings::ove_trace_register_name(id, name.as_ptr()) };
        REGISTERING.store(false, Ordering::Release);
        registered
    }
    #[cfg(not(has_trace))]
    {
        let _ = (id, name);
        false
    }
}

/// Emit a point mark on the current lane.
#[inline]
pub fn mark(id: u16) {
    #[cfg(has_trace)]
    unsafe {
        bindings::ove_trace_mark(id);
    }
    #[cfg(not(has_trace))]
    let _ = id;
}

/// Open a zone on the current lane.
///
/// Zones with the same id may nest.
#[inline]
pub fn zone_begin(id: u16) {
    #[cfg(has_trace)]
    unsafe {
        bindings::ove_trace_zone_begin(id);
    }
    #[cfg(not(has_trace))]
    let _ = id;
}

/// Close the innermost open zone with this id on the current lane.
#[inline]
pub fn zone_end(id: u16) {
    #[cfg(has_trace)]
    unsafe {
        bindings::ove_trace_zone_end(id);
    }
    #[cfg(not(has_trace))]
    let _ = id;
}

/// A scope guard that opens a zone on construction and closes it on drop.
///
/// The dashboard pairs records on the opening thread's lane, so `Zone`
/// implements neither `Send` nor `Sync`.
#[derive(Debug)]
pub struct Zone {
    id: u16,
    _not_send: PhantomData<*const ()>,
}

impl Zone {
    /// Open a zone with the given id.
    #[must_use]
    pub fn new(id: u16) -> Self {
        zone_begin(id);
        Self {
            id,
            _not_send: PhantomData,
        }
    }

    /// Close the zone early by consuming the guard.
    pub fn finish(self) {
        drop(self);
    }
}

impl Drop for Zone {
    fn drop(&mut self) {
        zone_end(self.id);
    }
}
