// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::RecursiveMutex;

fn test_create() {
    let mtx = RecursiveMutex::new().unwrap();
    drop(mtx);
}

fn test_lock_twice() {
    let mutex = RecursiveMutex::new().unwrap();
    mutex.with_lock(|| mutex.with_lock(|| Ok(()))).unwrap();
}

fn test_matching_unlocks() {
    let mutex = RecursiveMutex::new().unwrap();
    mutex
        .with_lock(|| mutex.with_lock(|| mutex.with_lock(|| Ok(()))))
        .unwrap();
    mutex.try_with_lock(|| Ok(())).unwrap();
}

fn test_raii_drop() {
    let mutex = RecursiveMutex::new().unwrap();
    mutex.with_lock(|| Ok(())).unwrap();
}

fn test_scope_auto_unlock() {
    let mutex = RecursiveMutex::new().unwrap();
    mutex.with_lock(|| mutex.with_lock(|| Ok(()))).unwrap();
    mutex.try_with_lock(|| Ok(())).unwrap();
}

fn test_scope_nested() {
    let mutex = RecursiveMutex::new().unwrap();
    mutex
        .with_lock(|| {
            mutex.with_lock(|| Ok(()))?;
            mutex.try_with_lock(|| Ok(()))
        })
        .unwrap();
    mutex.try_with_lock(|| Ok(())).unwrap();
}

fn test_mutex_debug_format() {
    let mutex = RecursiveMutex::new().unwrap();
    let text = format!("{mutex:?}");
    assert!(text.contains("RecursiveMutex"), "unexpected debug: {text}");
    assert!(text.contains("handle"), "unexpected debug: {text}");
}

pub fn run() -> (usize, usize) {
    run_suite(
        "RecursiveMutex",
        &[
            test_entry!(test_create),
            test_entry!(test_lock_twice),
            test_entry!(test_matching_unlocks),
            test_entry!(test_raii_drop),
            test_entry!(test_scope_auto_unlock),
            test_entry!(test_scope_nested),
            test_entry!(test_mutex_debug_format),
        ],
    )
}
