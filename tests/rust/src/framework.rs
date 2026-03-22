// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use std::panic;
use std::sync::atomic::{AtomicPtr, Ordering};

pub type TestFn = fn();

pub struct TestEntry {
    pub name: &'static str,
    pub func: TestFn,
}

/// Drop guard that resets an `AtomicPtr` to null when dropped.
/// Prevents dangling global pointers if a test panics.
pub struct PtrGuard<T: 'static> {
    ptr: &'static AtomicPtr<T>,
}

impl<T: 'static> PtrGuard<T> {
    pub fn new(ptr: &'static AtomicPtr<T>, val: *mut T) -> Self {
        ptr.store(val, Ordering::Release);
        Self { ptr }
    }
}

impl<T: 'static> Drop for PtrGuard<T> {
    fn drop(&mut self) {
        self.ptr.store(core::ptr::null_mut(), Ordering::Release);
    }
}

pub fn run_suite(suite_name: &str, tests: &[TestEntry]) -> (usize, usize) {
    let mut passed = 0usize;
    let mut failed = 0usize;

    println!("[==========] Running {} test(s) from {}", tests.len(), suite_name);

    for entry in tests {
        print!("[ RUN      ] {}::{}", suite_name, entry.name);
        println!();

        let result = panic::catch_unwind(panic::AssertUnwindSafe(entry.func));

        match result {
            Ok(()) => {
                println!("[       OK ] {}::{}", suite_name, entry.name);
                passed += 1;
            }
            Err(_) => {
                println!("[  FAILED  ] {}::{}", suite_name, entry.name);
                failed += 1;
            }
        }
    }

    println!(
        "[==========] {} test(s) from {} ran ({} passed, {} failed)",
        tests.len(),
        suite_name,
        passed,
        failed
    );

    if failed == 0 {
        println!("[  PASSED  ] {} test(s) from {}", passed, suite_name);
    } else {
        println!("[  FAILED  ] {} test(s) from {}", failed, suite_name);
    }

    (passed, failed)
}

#[macro_export]
macro_rules! test_entry {
    ($func:ident) => {
        $crate::framework::TestEntry {
            name: stringify!($func),
            func: $func,
        }
    };
}
