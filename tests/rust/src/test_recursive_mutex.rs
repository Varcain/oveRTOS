// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{RecursiveMutex, RecursiveMutexGuard};
use static_assertions::assert_not_impl_all;

// Same `!Send` invariant as `MutexGuard` — see test_mutex.rs.
assert_not_impl_all!(RecursiveMutexGuard<'static>: Send);

fn test_create() {
    let mtx = RecursiveMutex::new().unwrap();
    drop(mtx);
}

fn test_lock_twice() {
    let mtx = RecursiveMutex::new().unwrap();
    let _g1 = mtx.lock().unwrap();
    let _g2 = mtx.lock().unwrap();
}

fn test_matching_unlocks() {
    let mtx = RecursiveMutex::new().unwrap();
    {
        let _g1 = mtx.lock().unwrap();
        let _g2 = mtx.lock().unwrap();
        let _g3 = mtx.lock().unwrap();
    }
    let _g = mtx.try_lock().unwrap();
}

fn test_raii_drop() {
    {
        let mtx = RecursiveMutex::new().unwrap();
        let _g = mtx.lock().unwrap();
    }
}

fn test_guard_auto_unlock() {
    let mtx = RecursiveMutex::new().unwrap();
    {
        let _guard = mtx.lock().unwrap();
        // Recursive — can re-lock inside guard
        let _inner = mtx.lock().unwrap();
    }
    // Guard dropped — still one less lock level
    let _g = mtx.try_lock().unwrap();
}

fn test_guard_nested() {
    let mtx = RecursiveMutex::new().unwrap();
    {
        let _g1 = mtx.lock().unwrap();
        {
            let _g2 = mtx.lock().unwrap();
            // Two guard levels held
        }
        // One guard level remaining
    }
    // All released
    let _g = mtx.try_lock().unwrap();
}

fn test_guard_debug_format() {
    let mtx = RecursiveMutex::new().unwrap();
    let guard = mtx.lock().unwrap();
    let s = format!("{:?}", guard);
    assert!(s.contains("RecursiveMutexGuard"), "unexpected debug: {s}");
    assert!(s.contains("mutex"), "unexpected debug: {s}");
}

pub fn run() -> (usize, usize) {
    run_suite("RecursiveMutex", &[
        test_entry!(test_create),
        test_entry!(test_lock_twice),
        test_entry!(test_matching_unlocks),
        test_entry!(test_raii_drop),
        test_entry!(test_guard_auto_unlock),
        test_entry!(test_guard_nested),
        test_entry!(test_guard_debug_format),
    ])
}
