// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{StaticCell, StaticMut};

static TEST_CELL: StaticMut<i32> = StaticMut::new();

fn test_init_and_get() {
    // Fresh cell for each test — use a local approach
    static CELL: StaticMut<u64> = StaticMut::new();
    CELL.init(42);
    assert_eq!(*CELL.get(), 42);
    CELL.shutdown();
}

fn test_try_get_uninitialized() {
    static CELL: StaticMut<u32> = StaticMut::new();
    assert!(CELL.try_get().is_none());
}

fn test_try_get_initialized() {
    static CELL: StaticMut<u32> = StaticMut::new();
    CELL.init(99);
    assert_eq!(*CELL.try_get().unwrap(), 99);
    CELL.shutdown();
}

fn test_get_mut() {
    static CELL: StaticMut<[u8; 4]> = StaticMut::new();
    CELL.init([1, 2, 3, 4]);

    // SAFETY: single-threaded test
    let data = unsafe { CELL.get_mut() };
    data[0] = 10;
    data[3] = 40;

    assert_eq!(*CELL.get(), [10, 2, 3, 40]);
    CELL.shutdown();
}

fn test_shutdown_idempotent() {
    static CELL: StaticMut<i32> = StaticMut::new();
    CELL.init(1);
    CELL.shutdown();
    CELL.shutdown(); // should not panic
    assert!(CELL.try_get().is_none());
}

fn test_reinit_after_shutdown() {
    static CELL: StaticMut<i32> = StaticMut::new();
    CELL.init(1);
    assert_eq!(*CELL.get(), 1);
    CELL.shutdown();

    CELL.init(2);
    assert_eq!(*CELL.get(), 2);
    CELL.shutdown();
}

fn test_double_init_panics() {
    TEST_CELL.init(1);
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        TEST_CELL.init(2);
    }));
    TEST_CELL.shutdown();
    assert!(result.is_err(), "double init should panic");
}

fn test_get_uninitialized_panics() {
    static CELL: StaticMut<i32> = StaticMut::new();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _ = *CELL.get();
    }));
    assert!(result.is_err(), "get on uninitialized cell should panic");
}

/* ── StaticCell (separate type from StaticMut) ─────────────────────── */

static SC_TEST: StaticCell<i32> = StaticCell::new();

fn test_static_cell_try_get_both_branches() {
    static SC: StaticCell<u32> = StaticCell::new();
    // Pre-init: try_get → None
    assert!(SC.try_get().is_none());
    SC.init(123);
    // Post-init: try_get → Some
    assert_eq!(*SC.try_get().unwrap(), 123);
    SC.shutdown();
    // Post-shutdown: None again
    assert!(SC.try_get().is_none());
}

fn test_static_cell_double_init_panics() {
    SC_TEST.shutdown();
    SC_TEST.init(1);
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        SC_TEST.init(2);
    }));
    SC_TEST.shutdown();
    assert!(result.is_err(), "StaticCell double init should panic");
}

pub fn run() -> (usize, usize) {
    // Reset TEST_CELL for double_init test
    TEST_CELL.shutdown();

    run_suite(
        "StaticMut",
        &[
            test_entry!(test_init_and_get),
            test_entry!(test_try_get_uninitialized),
            test_entry!(test_try_get_initialized),
            test_entry!(test_get_mut),
            test_entry!(test_shutdown_idempotent),
            test_entry!(test_reinit_after_shutdown),
            test_entry!(test_double_init_panics),
            test_entry!(test_get_uninitialized_panics),
            test_entry!(test_static_cell_try_get_both_branches),
            test_entry!(test_static_cell_double_init_panics),
        ],
    )
}
