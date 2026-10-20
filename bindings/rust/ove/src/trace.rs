// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Named user trace markers and zones.
//!
//! Ids are application-defined `u16` values registered with [`name`]. The
//! wrappers are no-ops when the build has trace markers disabled
//! (`CONFIG_OVE_TRACE_MARKERS` off), as the C calls are. Names must be
//! NUL-terminated; the trace layer copies them, cut to 31 bytes, so they
//! do not have to live for the rest of the program — a C-string literal
//! (`c"draw"`) is the easiest way to build one. Register names from one
//! thread only: two threads registering at once can lose a name.
//!
//! Marks and zones may also be emitted from an interrupt handler; their
//! records then land on the interrupt lane.

#[cfg(has_trace)]
use crate::bindings;
use core::ffi::CStr;
use core::marker::PhantomData;

// SAFETY (module-wide contract for the `unsafe { bindings::ove_trace_*(...) }`
// FFI calls below): the functions take plain integers, and the one pointer
// (`name`) comes from a `&CStr`, so it is non-null and NUL-terminated for
// the duration of the call; the C side copies the name and keeps no
// reference to it.

/// Register a human-readable name for a user trace id.
///
/// The name is copied by the trace layer, so it need not outlive the
/// call. Call it from one thread only.
///
/// Returns `true` when the name was registered, `false` when it was
/// rejected (empty name, or the name table is full) or markers are
/// disabled.
#[inline]
#[must_use]
pub fn name(id: u16, name: &CStr) -> bool {
    #[cfg(has_trace)]
    {
        unsafe { bindings::ove_trace_register_name(id, name.as_ptr()) }
    }
    #[cfg(not(has_trace))]
    {
        let _ = (id, name);
        false
    }
}

/// Emit a point mark on the calling thread's lane.
#[inline]
pub fn mark(id: u16) {
    #[cfg(has_trace)]
    unsafe {
        bindings::ove_trace_mark(id);
    }
    #[cfg(not(has_trace))]
    let _ = id;
}

/// Open a named zone; pair with [`zone_end`] (or use [`Zone`]).
#[inline]
pub fn zone_begin(id: u16) {
    #[cfg(has_trace)]
    unsafe {
        bindings::ove_trace_zone_begin(id);
    }
    #[cfg(not(has_trace))]
    let _ = id;
}

/// Close the most recent zone with `id` on the calling thread: the
/// innermost one still open.
#[inline]
pub fn zone_end(id: u16) {
    #[cfg(has_trace)]
    unsafe {
        bindings::ove_trace_zone_end(id);
    }
    #[cfg(not(has_trace))]
    let _ = id;
}

/// RAII zone: opens on construction and closes when dropped.
///
/// A zone belongs to the thread that opened it (the end closes the zone
/// on the calling thread), so `Zone` is neither `Send` nor `Sync`.
#[derive(Debug)]
pub struct Zone {
    id: u16,
    active: bool,
    _not_send: PhantomData<*const ()>,
}

impl Zone {
    /// Open a zone with the given user id.
    #[must_use]
    pub fn new(id: u16) -> Self {
        zone_begin(id);
        Self {
            id,
            active: true,
            _not_send: PhantomData,
        }
    }

    /// Close the zone early. Dropping afterwards is a no-op.
    pub fn finish(mut self) {
        if self.active {
            zone_end(self.id);
            self.active = false;
        }
    }
}

impl Drop for Zone {
    fn drop(&mut self) {
        if self.active {
            zone_end(self.id);
        }
    }
}
