// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::Stream;

fn test_create_destroy() {
    let _s = Stream::<256>::new(1).unwrap();
}

fn test_send_receive() {
    let s = Stream::<256>::new(1).unwrap();
    let tx = [0xDE_u8, 0xAD, 0xBE, 0xEF];
    let sent = s.try_send_for(&tx, core::time::Duration::from_millis(100)).unwrap();
    assert_eq!(sent, 4);

    let mut rx = [0u8; 4];
    let received = s.try_recv_for(&mut rx, core::time::Duration::from_millis(100)).unwrap();
    assert_eq!(received, 4);
    assert_eq!(rx, tx);
}

fn test_bytes_available() {
    let s = Stream::<256>::new(1).unwrap();
    assert_eq!(s.bytes_available(), 0);

    let data = [1u8, 2, 3];
    s.try_send_for(&data, core::time::Duration::from_millis(100)).unwrap();
    assert!(s.bytes_available() >= 3);
}

fn test_reset() {
    let s = Stream::<256>::new(1).unwrap();
    let data = [1u8, 2, 3];
    s.try_send_for(&data, core::time::Duration::from_millis(100)).unwrap();
    assert!(s.bytes_available() > 0);

    s.reset().unwrap();
    assert_eq!(s.bytes_available(), 0);
}

fn test_send_from_isr() {
    let s = Stream::<256>::new(1).unwrap();
    let data = [0xAA_u8, 0xBB];
    let sent = s.send_from_isr(&data).unwrap();
    assert_eq!(sent, 2);
}

fn test_receive_from_isr() {
    let s = Stream::<256>::new(1).unwrap();
    let tx = [0x11_u8, 0x22];
    s.try_send_for(&tx, core::time::Duration::from_millis(100)).unwrap();

    let mut rx = [0u8; 2];
    let received = s.receive_from_isr(&mut rx).unwrap();
    assert_eq!(received, 2);
    assert_eq!(rx, tx);
}

fn test_raii_drop() {
    {
        let s = Stream::<128>::new(1).unwrap();
        s.try_send(&[1]).unwrap();
    }
}

pub fn run() -> (usize, usize) {
    run_suite("Stream", &[
        test_entry!(test_create_destroy),
        test_entry!(test_send_receive),
        test_entry!(test_bytes_available),
        test_entry!(test_reset),
        test_entry!(test_send_from_isr),
        test_entry!(test_receive_from_isr),
        test_entry!(test_raii_drop),
    ])
}
