// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_get_us_returns_ok() {
    let us = ove::time::get_us().unwrap();
    assert!(us > 0, "time should be non-zero");
}

fn test_get_us_monotonic() {
    let a = ove::time::get_us().unwrap();
    let b = ove::time::get_us().unwrap();
    assert!(b >= a, "time should be monotonic");
}

fn test_delay_ms() {
    let before = ove::time::get_us().unwrap();
    ove::time::delay_ms(50);
    let after = ove::time::get_us().unwrap();
    let elapsed_ms = (after - before) / 1000;
    assert!(elapsed_ms >= 40, "delay_ms(50) took only {} ms", elapsed_ms);
}

fn test_delay_us() {
    let before = ove::time::get_us().unwrap();
    ove::time::delay_us(10_000);
    let after = ove::time::get_us().unwrap();
    let elapsed_us = after - before;
    assert!(
        elapsed_us >= 5_000,
        "delay_us(10000) took only {} us",
        elapsed_us
    );
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Time",
        &[
            test_entry!(test_get_us_returns_ok),
            test_entry!(test_get_us_monotonic),
            test_entry!(test_delay_ms),
            test_entry!(test_delay_us),
        ],
    )
}
