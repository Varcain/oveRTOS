// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use core::ffi::{c_char, c_void};
use std::{ffi::CStr, time::Duration};

unsafe extern "C" {
    fn spi_test_raw(bits: u32) -> *mut c_void;
    fn spi_test_raw_close();
    fn spi_test_reset();
    fn spi_test_trace() -> *const c_char;
    fn spi_test_stat(index: u32) -> u32;
}

fn main() {
    let case = std::env::args()
        .nth(1)
        .expect("tx_short, rx_short or trait_rx_short");
    assert!(["tx_short", "rx_short", "trait_rx_short"].contains(&case.as_str()));
    let raw = unsafe { spi_test_raw(8) };
    assert!(!raw.is_null());
    // The C fixture owns stable backing until the matching close below.
    let mut spi = unsafe { ove::Spi::from_handle(raw.cast()) };
    unsafe { spi_test_reset() };
    let tx = [1u8, 2, 0xe1, 0xe2];
    let mut rx = [0xcc; 4];
    let result = match case.as_str() {
        "tx_short" => spi.transfer(None, &tx[..2], &mut rx, Duration::from_millis(20)),
        "trait_rx_short" => embedded_hal::spi::SpiBus::transfer(&mut spi, &mut rx[..1], &tx),
        _ => spi.transfer(None, &tx, &mut rx[..1], Duration::from_millis(20)),
    };
    let trace = unsafe { CStr::from_ptr(spi_test_trace()) }
        .to_str()
        .unwrap()
        .to_owned();
    let invalid = unsafe { spi_test_stat(10) };
    drop(spi);
    unsafe { spi_test_raw_close() };
    assert!(result.is_ok());
    assert_eq!(invalid, 0);
    if case == "tx_short" {
        assert_eq!(
            rx,
            [1, 2, 0xa5, 0xa5],
            "TX subslice sentinel bytes reached RX"
        );
    } else {
        assert_eq!(
            rx,
            [1, 0xcc, 0xcc, 0xcc],
            "C changed bytes outside the Rust RX subslice"
        );
    }
    assert_eq!(
        trace, "LHHU",
        "one real mutex interval covers both bounded segments"
    );
    println!("{case}: production Rust/C spans and mutex trace pass");
}
