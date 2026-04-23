// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

/* ── BSP-level wrapper ──────────────────────────────────────────────── */

fn test_board_init() {
    ove::bsp::board_init().unwrap();
}

fn test_board_init_idempotent() {
    ove::bsp::board_init().unwrap();
    ove::bsp::board_init().unwrap();
}

/* ── High-level ove::board API ──────────────────────────────────────── */

fn test_board_high_level_init() {
    ove::board::init().unwrap();
}

fn test_board_high_level_name() {
    let name = ove::board::name();
    assert!(!name.is_empty());
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Board",
        &[
            test_entry!(test_board_init),
            test_entry!(test_board_init_idempotent),
            test_entry!(test_board_high_level_init),
            test_entry!(test_board_high_level_name),
        ],
    )
}
