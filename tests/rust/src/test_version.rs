// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

use ove::version;

fn test_version_string_spells_numbers() {
    let spelled = format!("{}.{}.{}", version::MAJOR, version::MINOR, version::PATCH);
    assert_eq!(spelled, version::VERSION_STR);
}

fn test_version_packing_orders() {
    assert_eq!(
        version::VERSION,
        version::number(version::MAJOR, version::MINOR, version::PATCH)
    );
    assert!(version::number(0, 9, 9) < version::number(1, 0, 0));
    assert!(version::number(1, 2, 255) < version::number(1, 3, 0));
    assert!(version::VERSION >= version::number(0, 1, 0));
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Version",
        &[
            test_entry!(test_version_string_spells_numbers),
            test_entry!(test_version_packing_orders),
        ],
    )
}
