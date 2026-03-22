// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{RecursiveMutex, WAIT_FOREVER};

fn test_create() {
    let mtx = RecursiveMutex::new().unwrap();
    drop(mtx);
}

fn test_lock_twice() {
    let mtx = RecursiveMutex::new().unwrap();
    mtx.lock(WAIT_FOREVER).unwrap();
    mtx.lock(WAIT_FOREVER).unwrap();
    mtx.unlock();
    mtx.unlock();
}

fn test_matching_unlocks() {
    let mtx = RecursiveMutex::new().unwrap();
    for _ in 0..3 {
        mtx.lock(WAIT_FOREVER).unwrap();
    }
    for _ in 0..3 {
        mtx.unlock();
    }
    mtx.lock(0).unwrap();
    mtx.unlock();
}

fn test_raii_drop() {
    {
        let mtx = RecursiveMutex::new().unwrap();
        mtx.lock(WAIT_FOREVER).unwrap();
        mtx.unlock();
    }
}

fn test_guard_auto_unlock() {
    let mtx = RecursiveMutex::new().unwrap();
    {
        let _guard = mtx.guard(WAIT_FOREVER).unwrap();
        // Recursive — can re-lock inside guard
        mtx.lock(WAIT_FOREVER).unwrap();
        mtx.unlock();
    }
    // Guard dropped — still one less lock level
    mtx.lock(0).unwrap();
    mtx.unlock();
}

fn test_guard_nested() {
    let mtx = RecursiveMutex::new().unwrap();
    {
        let _g1 = mtx.guard(WAIT_FOREVER).unwrap();
        {
            let _g2 = mtx.guard(WAIT_FOREVER).unwrap();
            // Two guard levels held
        }
        // One guard level remaining
    }
    // All released
    mtx.lock(0).unwrap();
    mtx.unlock();
}

pub fn run() -> (usize, usize) {
    run_suite("RecursiveMutex", &[
        test_entry!(test_create),
        test_entry!(test_lock_twice),
        test_entry!(test_matching_unlocks),
        test_entry!(test_raii_drop),
        test_entry!(test_guard_auto_unlock),
        test_entry!(test_guard_nested),
    ])
}
