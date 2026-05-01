// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Bench-only DWT cycle-counter timer (Rust binding).
//!
//! Mirrors `tests/benchmarks/c/include/bench_cyccnt.h`.  On ARMv7-M /
//! ARMv7E-M (Cortex-M3/M4/M7) `read()` is a single `core::ptr::read_volatile`
//! of `0xE0001004` — exactly one ARM `LDR` after `-O` codegen, the same
//! shape the C inline emits.
//!
//! On non-ARM (POSIX, sim, host tests) the module compiles to stubs; the
//! C harness's fallback (`ove_time_get_ns`) is what runs there.
//!
//! Lives inside the bench tree only.  Not part of the public `ove` crate.

#![allow(dead_code)]

#[cfg(any(target_arch = "arm", all(target_arch = "thumb")))]
mod arm {
    use core::ptr::{read_volatile, write_volatile};

    /// CPU clock used for cycle→ns conversion. Override at compile time
    /// with `RUSTFLAGS="--cfg bench_cyccnt_hz_<N>"` if the board is not
    /// 216 MHz; otherwise the harness's C-side conversion is authoritative.
    pub const HZ: u32 = 216_000_000;

    const DEMCR: *mut u32 = 0xE000_EDFC as *mut u32;
    const DWT_CTRL: *mut u32 = 0xE000_1000 as *mut u32;
    const DWT_CYCCNT: *mut u32 = 0xE000_1004 as *mut u32;

    /// Enable the DWT cycle counter. Idempotent — safe to call repeatedly.
    /// Called once at app start; the C harness calls its own equivalent
    /// before any measurement.
    #[inline]
    pub fn init() {
        // SAFETY: ARMv7-M architecturally-mapped MMIO; bench-only; the
        // single-CPU bench app is the only writer to these registers.
        unsafe {
            write_volatile(DEMCR, read_volatile(DEMCR) | (1 << 24));
            write_volatile(DWT_CYCCNT, 0);
            write_volatile(DWT_CTRL, read_volatile(DWT_CTRL) | 1);
        }
    }

    /// Read the 32-bit free-running cycle counter. One `LDR` post-codegen.
    #[inline(always)]
    pub fn read() -> u32 {
        // SAFETY: aligned 4-byte read of an ARMv7-M architecturally-defined
        // register; the value is racy by design (free-running counter).
        unsafe { read_volatile(DWT_CYCCNT) }
    }

    /// Convert a 32-bit cycle delta (modulo-2^32 unsigned subtract) to ns.
    #[inline]
    pub fn diff_ns(start: u32, end: u32) -> u64 {
        let cycles = end.wrapping_sub(start);
        (cycles as u64) * 1_000_000_000 / (HZ as u64)
    }
}

#[cfg(any(target_arch = "arm", all(target_arch = "thumb")))]
pub use arm::{HZ, diff_ns, init, read};

#[cfg(not(any(target_arch = "arm", all(target_arch = "thumb"))))]
mod stub {
    pub const HZ: u32 = 0;
    #[inline]
    pub fn init() {}
    #[inline]
    pub fn read() -> u32 {
        0
    }
    #[inline]
    pub fn diff_ns(_start: u32, _end: u32) -> u64 {
        0
    }
}

#[cfg(not(any(target_arch = "arm", all(target_arch = "thumb"))))]
pub use stub::{HZ, diff_ns, init, read};

/// True iff this build's read() is backed by DWT (i.e. ARMv7-M).
pub const AVAILABLE: bool = cfg!(any(target_arch = "arm", target_arch = "thumb"));
