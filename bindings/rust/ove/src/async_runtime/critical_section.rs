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
//!
//! The provider uses the backend's native exclusion domain. On supported
//! single-core FreeRTOS Cortex-M ports this is kernel-aware IRQ exclusion;
//! higher-urgency interrupts cannot participate. The C boundary stops
//! unsupported entry before returning a token, including with assertions off.
//! Restricted MPU V1 calls require a serviceable priority-zero supervisor
//! call with PRIMASK and FAULTMASK clear; native setup must establish this.
//!
//! FreeRTOS restores captured masks and caller privilege at wrapper boundaries.
//! Native calls inside the body can still lower a stronger incoming BASEPRI
//! or clear PRIMASK. This provider promises neither global IRQ masking nor
//! continuous stronger-mask protection through arbitrary native body calls.
//! Other backends retain their own native execution-context contracts.

use ::critical_section::{Impl, RawRestoreState};

use crate::bindings;

struct OveCriticalSection;
::critical_section::set_impl!(OveCriticalSection);

unsafe impl Impl for OveCriticalSection {
    unsafe fn acquire() -> RawRestoreState {
        // SAFETY: supported native configurations establish the exclusion domain.
        // FreeRTOS checks eligible entry before returning. Its documented native
        // setup preconditions also apply to callers through this provider.
        // The opaque u64 restore state passes through unchanged.
        unsafe { bindings::ove_irq_lock() as RawRestoreState }
    }

    unsafe fn release(state: RawRestoreState) {
        // SAFETY: paired with acquire above; the C-side impl handles
        // nested restore correctly.
        unsafe { bindings::ove_irq_unlock(state as bindings::ove_irq_key_t) }
    }
}
