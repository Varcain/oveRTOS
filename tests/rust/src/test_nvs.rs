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
    ove::nvs::write(c"key1", b"hello").unwrap();

    let mut buf = [0u8; 32];
    let len = ove::nvs::read(c"key1", &mut buf).unwrap();
    assert_eq!(len, 5);
    assert_eq!(&buf[..5], b"hello");
}

fn test_read_nonexistent_key() {
    ove::nvs::init().unwrap();
    let mut buf = [0u8; 32];
    let result = ove::nvs::read(c"nokey", &mut buf);
    assert!(result.is_err(), "reading missing key should fail");
}

fn test_overwrite() {
    ove::nvs::init().unwrap();
    ove::nvs::write(c"over", b"first").unwrap();
    ove::nvs::write(c"over", b"second").unwrap();

    let mut buf = [0u8; 32];
    let len = ove::nvs::read(c"over", &mut buf).unwrap();
    assert_eq!(len, 6);
    assert_eq!(&buf[..6], b"second");
}

fn test_erase() {
    ove::nvs::init().unwrap();
    ove::nvs::write(c"del", b"data").unwrap();
    ove::nvs::erase(c"del").unwrap();

    let mut buf = [0u8; 32];
    let result = ove::nvs::read(c"del", &mut buf);
    assert!(result.is_err(), "reading erased key should fail");
}

fn test_erase_nonexistent() {
    ove::nvs::init().unwrap();
    let result = ove::nvs::erase(c"nope");
    assert!(result.is_ok(), "erasing a missing key is a no-op");
}

fn test_binary_data() {
    ove::nvs::init().unwrap();
    let data: [u8; 4] = [0xFF, 0x00, 0xAB, 0x42];
    ove::nvs::write(c"bin", &data).unwrap();

    let mut buf = [0u8; 4];
    let len = ove::nvs::read(c"bin", &mut buf).unwrap();
    assert_eq!(len, 4);
    assert_eq!(buf, data);
}

fn test_runtime_key() {
    use core::fmt::Write;

    // A key built at run time, as a C string: the stored key is exactly
    // the formatted text, no more.
    ove::nvs::init().unwrap();
    let mut storage = [0u8; 16];
    let mut key = ove::FmtBuf::new(&mut storage);
    write!(key, "slot{}", 3).unwrap();
    ove::nvs::write(key.as_c_str().unwrap(), b"v3").unwrap();

    let mut buf = [0u8; 8];
    let len = ove::nvs::read(c"slot3", &mut buf).unwrap();
    assert_eq!(&buf[..len], b"v3");
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
            test_entry!(test_runtime_key),
        ],
    )
}
