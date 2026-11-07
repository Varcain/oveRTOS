// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Tests for scoped access to data carried by `Mutex<T>`.
//!
//! `test_mutex.rs` exercises the `Mutex<()>` (no-data) shape — these
//! tests pin down the `Deref`/`DerefMut` access pattern, `get_mut`,
//! and `into_inner`.

use crate::framework::run_suite;
use crate::test_entry;
use ove::Mutex;

fn test_deref_reads_data() {
    let mutex = Mutex::new(42i32).unwrap();
    mutex
        .with_lock(|value| {
            assert_eq!(*value, 42);
            Ok(())
        })
        .unwrap();
}

fn test_deref_mut_writes_data() {
    let mutex = Mutex::new(0i32).unwrap();
    mutex
        .with_lock(|mut value| {
            *value = 99;
            Ok(())
        })
        .unwrap();
    mutex
        .with_lock(|value| {
            assert_eq!(*value, 99);
            Ok(())
        })
        .unwrap();
}

fn test_get_mut_skips_lock() {
    let mut mutex = Mutex::new(7i32).unwrap();
    *mutex.get_mut() = 11;
    mutex
        .with_lock(|value| {
            assert_eq!(*value, 11);
            Ok(())
        })
        .unwrap();
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
    let mutex = Mutex::new(Point { x: 3, y: 4 }).unwrap();
    mutex
        .with_lock(|mut point| {
            point.x = 10;
            point.y = 20;
            Ok(())
        })
        .unwrap();
    mutex
        .with_lock(|point| {
            assert_eq!(*point, Point { x: 10, y: 20 });
            Ok(())
        })
        .unwrap();
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
