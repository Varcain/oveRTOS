// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

/* ── BSP-level wrapper ──────────────────────────────────────────────── */

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

/* ── High-level ove::led API ────────────────────────────────────────── */

fn test_led_high_level_set() {
    ove::led::set(0, true);
    ove::led::set(0, false);
    ove::led::set(1, true);
}

fn test_led_high_level_toggle() {
    ove::led::toggle(0);
    ove::led::toggle(1);
}

fn test_led_high_level_count() {
    let _n = ove::led::count();
    // Stub may report 0 or some positive number; just exercise the path.
}

pub fn run() -> (usize, usize) {
    run_suite(
        "LED",
        &[
            test_entry!(test_led_set_no_panic),
            test_entry!(test_led_toggle_no_panic),
            test_entry!(test_led_set_out_of_range),
            test_entry!(test_led_high_level_set),
            test_entry!(test_led_high_level_toggle),
            test_entry!(test_led_high_level_count),
        ],
    )
}
