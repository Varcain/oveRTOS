// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_led_set_no_panic() {
    ove::bsp::led_set(0, true);
    ove::bsp::led_set(0, false);
    ove::bsp::led_set(1, true);
    ove::bsp::led_set(7, false);
}

fn test_led_toggle_no_panic() {
    ove::bsp::led_toggle(0);
    ove::bsp::led_toggle(0);
    ove::bsp::led_toggle(1);
}

fn test_led_set_out_of_range() {
    // Stub silently ignores out-of-range LEDs — should not panic
    ove::bsp::led_set(100, true);
    ove::bsp::led_toggle(100);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "LED",
        &[
            test_entry!(test_led_set_no_panic),
            test_entry!(test_led_toggle_no_panic),
            test_entry!(test_led_set_out_of_range),
        ],
    )
}
