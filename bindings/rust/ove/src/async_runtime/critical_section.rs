// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Global `critical_section::Impl` backed by `ove_irq_lock` /
//! `ove_irq_unlock`.
//!
//! The `restore-state-u64` feature on `critical-section` matches our
//! `ove_irq_key_t` (uint64_t) so the cookie passes through
//! unchanged. Setting this provider claims the global critical-section
//! impl for the entire dep graph — users who pull in another crate
//! that also calls `critical_section::set_impl!` will get a link error,
//! which is the intended behaviour (only one impl can exist).

use ::critical_section::{Impl, RawRestoreState};

use crate::bindings;

struct OveCriticalSection;
::critical_section::set_impl!(OveCriticalSection);

unsafe impl Impl for OveCriticalSection {
    unsafe fn acquire() -> RawRestoreState {
        // SAFETY: ove_irq_lock is safe to call from any context per the
        // C-side contract (it nests via depth counter on POSIX, real
        // IRQ-disable on embedded backends). Returns an opaque cookie
        // which we re-interpret as RawRestoreState; the
        // `restore-state-u64` feature in Cargo.toml pins this to u64
        // matching ove_irq_key_t.
        unsafe { bindings::ove_irq_lock() as RawRestoreState }
    }

    unsafe fn release(state: RawRestoreState) {
        // SAFETY: paired with acquire above; the C-side impl handles
        // nested restore correctly.
        unsafe { bindings::ove_irq_unlock(state as bindings::ove_irq_key_t) }
    }
}
