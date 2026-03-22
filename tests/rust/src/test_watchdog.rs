// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::Watchdog;

fn test_create_destroy() {
    let _wdog = Watchdog::new(5000).unwrap();
    // Drop triggers destroy — should not panic
}

fn test_start() {
    let wdog = Watchdog::new(5000).unwrap();
    wdog.start().unwrap();
}

fn test_feed() {
    let wdog = Watchdog::new(5000).unwrap();
    wdog.start().unwrap();
    wdog.feed().unwrap();
    wdog.feed().unwrap();
}

fn test_raii_drop() {
    {
        let wdog = Watchdog::new(1000).unwrap();
        wdog.start().unwrap();
        wdog.feed().unwrap();
        // wdog dropped here — destroy called
    }
    // No crash after drop
}

fn test_feed_multiple() {
    let wdog = Watchdog::new(5000).unwrap();
    wdog.start().unwrap();
    for _ in 0..10 {
        wdog.feed().unwrap();
    }
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Watchdog",
        &[
            test_entry!(test_create_destroy),
            test_entry!(test_start),
            test_entry!(test_feed),
            test_entry!(test_raii_drop),
            test_entry!(test_feed_multiple),
        ],
    )
}
