// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_init() {
    ove::nvs::init().unwrap();
}

fn test_write_read_roundtrip() {
    ove::nvs::init().unwrap();
    ove::nvs::write(b"key1\0", b"hello").unwrap();

    let mut buf = [0u8; 32];
    let len = ove::nvs::read(b"key1\0", &mut buf).unwrap();
    assert_eq!(len, 5);
    assert_eq!(&buf[..5], b"hello");
}

fn test_read_nonexistent_key() {
    ove::nvs::init().unwrap();
    let mut buf = [0u8; 32];
    let result = ove::nvs::read(b"nokey\0", &mut buf);
    assert!(result.is_err(), "reading missing key should fail");
}

fn test_overwrite() {
    ove::nvs::init().unwrap();
    ove::nvs::write(b"over\0", b"first").unwrap();
    ove::nvs::write(b"over\0", b"second").unwrap();

    let mut buf = [0u8; 32];
    let len = ove::nvs::read(b"over\0", &mut buf).unwrap();
    assert_eq!(len, 6);
    assert_eq!(&buf[..6], b"second");
}

fn test_erase() {
    ove::nvs::init().unwrap();
    ove::nvs::write(b"del\0", b"data").unwrap();
    ove::nvs::erase(b"del\0").unwrap();

    let mut buf = [0u8; 32];
    let result = ove::nvs::read(b"del\0", &mut buf);
    assert!(result.is_err(), "reading erased key should fail");
}

fn test_erase_nonexistent() {
    ove::nvs::init().unwrap();
    let result = ove::nvs::erase(b"nope\0");
    assert!(result.is_err(), "erasing missing key should fail");
}

fn test_binary_data() {
    ove::nvs::init().unwrap();
    let data: [u8; 4] = [0xFF, 0x00, 0xAB, 0x42];
    ove::nvs::write(b"bin\0", &data).unwrap();

    let mut buf = [0u8; 4];
    let len = ove::nvs::read(b"bin\0", &mut buf).unwrap();
    assert_eq!(len, 4);
    assert_eq!(buf, data);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "NVS",
        &[
            test_entry!(test_init),
            test_entry!(test_write_read_roundtrip),
            test_entry!(test_read_nonexistent_key),
            test_entry!(test_overwrite),
            test_entry!(test_erase),
            test_entry!(test_erase_nonexistent),
            test_entry!(test_binary_data),
        ],
    )
}
