// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Compile-only tests pinning the `embedded-hal` 1.0 + `embedded-io`
//! 0.6 trait impls on the ove peripheral / I/O surface.
//!
//! In this build (stub) we can exercise: `Delay`, `gpio::OutputPin`,
//! `gpio::InputPin`, `Stream<N>` (Read+Write), `fs::File` (Read+Write).
//! The I2C / SPI / UART impls live in the binding crate behind
//! `#[cfg(has_i2c)]` / `#[cfg(has_spi)]` / `#[cfg(has_uart)]` and
//! compile automatically when those peripherals are enabled in the
//! substrate config — they're tested implicitly via downstream app
//! builds that enable them.

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
fn drives_io_read<R: embedded_io::Read>(_: &mut R) {}
fn drives_io_write<W: embedded_io::Write>(_: &mut W) {}

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

fn test_stream_io_impl_compiles() {
    // Construct a real stream — works on the stub.  256-byte buffer,
    // trigger 1.
    let mut s = ove::Stream::<256>::new(1).unwrap();
    drives_io_read(&mut s);
    drives_io_write(&mut s);
}

fn test_file_io_impl_compiles() {
    // Don't actually open a file — the stub backend has no FS.
    // Use the same dead-branch trick as the GPIO pins.
    if false {
        let mut f: ove::fs::File = unsafe { core::mem::zeroed() };
        drives_io_read(&mut f);
        drives_io_write(&mut f);
    }
}

fn test_io_error_kind_mappings() {
    use embedded_io::Error as _;
    use ove::Error;
    assert!(matches!(
        Error::Timeout.kind(),
        embedded_io::ErrorKind::TimedOut
    ));
    assert!(matches!(
        Error::NoMemory.kind(),
        embedded_io::ErrorKind::OutOfMemory
    ));
    assert!(matches!(
        Error::NotFound.kind(),
        embedded_io::ErrorKind::NotFound
    ));
    assert!(matches!(
        Error::InvalidParam.kind(),
        embedded_io::ErrorKind::InvalidInput
    ));
    assert!(matches!(
        Error::NotSupported.kind(),
        embedded_io::ErrorKind::Unsupported
    ));
    assert!(matches!(
        Error::NetClosed.kind(),
        embedded_io::ErrorKind::ConnectionAborted
    ));
}

pub fn run() -> (usize, usize) {
    run_suite(
        "embedded-hal+io",
        &[
            test_entry!(test_delay_impl_compiles),
            test_entry!(test_output_pin_impl_compiles),
            test_entry!(test_input_pin_impl_compiles),
            test_entry!(test_delay_methods_run),
            test_entry!(test_stream_io_impl_compiles),
            test_entry!(test_file_io_impl_compiles),
            test_entry!(test_io_error_kind_mappings),
        ],
    )
}
