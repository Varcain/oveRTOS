// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Tests for the data-carrying `Mutex<T>` surface introduced in B3.
//!
//! `test_mutex.rs` exercises the `Mutex<()>` (no-data) shape — these
//! tests pin down the `Deref`/`DerefMut` access pattern, `get_mut`,
//! and `into_inner`.

use crate::framework::run_suite;
use crate::test_entry;
use ove::Mutex;

fn test_deref_reads_data() {
    let mtx = Mutex::new(42i32).unwrap();
    let g = mtx.lock().unwrap();
    assert_eq!(*g, 42);
}

fn test_deref_mut_writes_data() {
    let mtx = Mutex::new(0i32).unwrap();
    {
        let mut g = mtx.lock().unwrap();
        *g = 99;
    }
    let g = mtx.lock().unwrap();
    assert_eq!(*g, 99);
}

fn test_get_mut_skips_lock() {
    // get_mut takes &mut self; no lock needed because exclusive access
    // is statically proven.
    let mut mtx = Mutex::new(7i32).unwrap();
    *mtx.get_mut() = 11;
    let g = mtx.lock().unwrap();
    assert_eq!(*g, 11);
}

fn test_into_inner_extracts_value() {
    let mtx = Mutex::new([1u8, 2, 3, 4]).unwrap();
    let v = mtx.into_inner().unwrap();
    assert_eq!(v, [1, 2, 3, 4]);
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Point {
    x: i32,
    y: i32,
}

fn test_struct_field_access() {
    let mtx = Mutex::new(Point { x: 3, y: 4 }).unwrap();
    {
        let mut g = mtx.lock().unwrap();
        g.x = 10;
        g.y = 20;
    }
    let g = mtx.lock().unwrap();
    assert_eq!(*g, Point { x: 10, y: 20 });
}

fn test_drop_runs_t_destructor() {
    use std::sync::atomic::{AtomicI32, Ordering};
    static DROPS: AtomicI32 = AtomicI32::new(0);

    struct Tracker;
    impl Drop for Tracker {
        fn drop(&mut self) {
            DROPS.fetch_add(1, Ordering::SeqCst);
        }
    }

    DROPS.store(0, Ordering::SeqCst);
    {
        let _mtx = Mutex::new(Tracker).unwrap();
    }
    // Mutex's Drop must run T's Drop too.
    assert_eq!(DROPS.load(Ordering::SeqCst), 1);
}

fn test_into_inner_does_not_double_drop() {
    use std::sync::atomic::{AtomicI32, Ordering};
    static DROPS: AtomicI32 = AtomicI32::new(0);

    struct Tracker;
    impl Drop for Tracker {
        fn drop(&mut self) {
            DROPS.fetch_add(1, Ordering::SeqCst);
        }
    }

    DROPS.store(0, Ordering::SeqCst);
    let mtx = Mutex::new(Tracker).unwrap();
    let val = mtx.into_inner().unwrap();
    // Tracker not yet dropped — we still own it via `val`.
    assert_eq!(DROPS.load(Ordering::SeqCst), 0);
    drop(val);
    // Now dropped exactly once.
    assert_eq!(DROPS.load(Ordering::SeqCst), 1);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Mutex<T>",
        &[
            test_entry!(test_deref_reads_data),
            test_entry!(test_deref_mut_writes_data),
            test_entry!(test_get_mut_skips_lock),
            test_entry!(test_into_inner_extracts_value),
            test_entry!(test_struct_field_access),
            test_entry!(test_drop_runs_t_destructor),
            test_entry!(test_into_inner_does_not_double_drop),
        ],
    )
}
