// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_display_width() {
    let _w = ove::lvgl::display::width();
}

fn test_display_height() {
    let _h = ove::lvgl::display::height();
}

fn test_display_dpi() {
    let _d = ove::lvgl::display::dpi();
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Lvgl",
        &[
            test_entry!(test_display_width),
            test_entry!(test_display_height),
            test_entry!(test_display_dpi),
        ],
    )
}
