// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_topic_str_from_bytes_accepts_utf8() {
    assert_eq!(
        ove::__test_support::mqtt_topic_str_from_bytes(b"sensors/temp"),
        Some("sensors/temp")
    );
}

fn test_topic_str_from_bytes_rejects_invalid_utf8() {
    assert_eq!(
        ove::__test_support::mqtt_topic_str_from_bytes(b"sensors/\xff"),
        None
    );
}

fn test_callback_bytes_accepts_null_for_empty_slice() {
    let bytes = unsafe { ove::__test_support::mqtt_callback_bytes(core::ptr::null::<u8>(), 0) };

    assert_eq!(bytes, Some(&[][..]));
}

fn test_callback_bytes_rejects_null_for_non_empty_slice() {
    let bytes = unsafe { ove::__test_support::mqtt_callback_bytes(core::ptr::null::<u8>(), 1) };

    assert_eq!(bytes, None);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "NetMqtt",
        &[
            test_entry!(test_topic_str_from_bytes_accepts_utf8),
            test_entry!(test_topic_str_from_bytes_rejects_invalid_utf8),
            test_entry!(test_callback_bytes_accepts_null_for_empty_slice),
            test_entry!(test_callback_bytes_rejects_null_for_non_empty_slice),
        ],
    )
}
