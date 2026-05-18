// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Compile-only tests pinning the `embedded-hal` 1.0 trait impls on
//! `ove::Delay`, `ove::gpio::OutputPin` and `ove::gpio::InputPin`.
//!
//! The I2C / SPI / UART impls live in the binding crate behind
//! `#[cfg(has_i2c)]` / `#[cfg(has_spi)]` / `#[cfg(has_uart)]` and are
//! compiled automatically when those peripherals are enabled in the
//! substrate config.  The stub build used here doesn't have them
//! enabled, so they're tested implicitly via the app builds (Zephyr
//! ARM zeroheap example rust + POSIX heap example rust) that do
//! include them.

use crate::framework::run_suite;
use crate::test_entry;

use embedded_hal::delay::DelayNs;
use embedded_hal::digital;

use ove::Delay;
use ove::gpio::{InputPin, OutputPin};

// Generic functions that bound on the relevant trait — calling these
// with the ove types proves the trait impls compile.

fn drives_output_pin<P: digital::OutputPin>(_: &mut P) {}
fn drives_input_pin<P: digital::InputPin>(_: &mut P) {}
fn drives_delay<D: DelayNs>(_: &mut D) {}

fn test_delay_impl_compiles() {
    let mut d = Delay;
    drives_delay(&mut d);
}

fn test_output_pin_impl_compiles() {
    // Don't actually construct — `OutputPin::new` configures the pin,
    // which fails on the stub.  Instead fabricate an instance for the
    // type-binding check alone.  The branch is dead at runtime; rustc
    // still type-checks it.
    if false {
        // SAFETY: never reached.
        let mut p: OutputPin = unsafe { core::mem::zeroed() };
        drives_output_pin(&mut p);
    }
}

fn test_input_pin_impl_compiles() {
    if false {
        let mut p: InputPin = unsafe { core::mem::zeroed() };
        drives_input_pin(&mut p);
    }
}

fn test_delay_methods_run() {
    // Smoke-test the actual code path — delay_ns(0) is a no-op, and
    // delay_ns(1..1000) should round up to 1us (no panic).
    let mut d = Delay;
    d.delay_ns(0);
    d.delay_ns(500);
    d.delay_us(1);
    // delay_ms(0) is also safe; substrate handles it.
    d.delay_ms(0);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "embedded-hal",
        &[
            test_entry!(test_delay_impl_compiles),
            test_entry!(test_output_pin_impl_compiles),
            test_entry!(test_input_pin_impl_compiles),
            test_entry!(test_delay_methods_run),
        ],
    )
}
