// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Timer, Thread};
use std::sync::atomic::{AtomicI32, Ordering};

static ONESHOT_COUNT: AtomicI32 = AtomicI32::new(0);
static PERIODIC_COUNT: AtomicI32 = AtomicI32::new(0);

fn oneshot_cb() {
    ONESHOT_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn periodic_cb() {
    PERIODIC_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn test_create_destroy_oneshot() {
    let _t = Timer::new(oneshot_cb, 100, true).unwrap();
}

fn test_create_destroy_periodic() {
    let _t = Timer::new(periodic_cb, 50, false).unwrap();
}

fn test_oneshot_fires_once() {
    ONESHOT_COUNT.store(0, Ordering::SeqCst);
    let t = Timer::new(oneshot_cb, 30, true).unwrap();
    t.start().unwrap();
    Thread::sleep_ms(200);
    assert_eq!(ONESHOT_COUNT.load(Ordering::SeqCst), 1);
}

fn test_periodic_fires_multiple() {
    PERIODIC_COUNT.store(0, Ordering::SeqCst);
    let t = Timer::new(periodic_cb, 30, false).unwrap();
    t.start().unwrap();
    Thread::sleep_ms(250);
    t.stop().unwrap();
    assert!(PERIODIC_COUNT.load(Ordering::SeqCst) >= 3);
}

fn test_stop_prevents_callbacks() {
    PERIODIC_COUNT.store(0, Ordering::SeqCst);
    let t = Timer::new(periodic_cb, 20, false).unwrap();
    t.start().unwrap();
    Thread::sleep_ms(100);
    t.stop().unwrap();
    let count_after_stop = PERIODIC_COUNT.load(Ordering::SeqCst);
    Thread::sleep_ms(150);
    assert!(PERIODIC_COUNT.load(Ordering::SeqCst) <= count_after_stop + 1);
}

fn test_reset_restarts() {
    PERIODIC_COUNT.store(0, Ordering::SeqCst);
    let t = Timer::new(periodic_cb, 50, false).unwrap();
    t.start().unwrap();
    Thread::sleep_ms(80);
    let before_reset = PERIODIC_COUNT.load(Ordering::SeqCst);
    t.reset().unwrap();
    Thread::sleep_ms(200);
    assert!(PERIODIC_COUNT.load(Ordering::SeqCst) > before_reset);
    t.stop().unwrap();
}

fn test_double_start() {
    PERIODIC_COUNT.store(0, Ordering::SeqCst);
    let t = Timer::new(periodic_cb, 30, false).unwrap();
    t.start().unwrap();
    t.start().unwrap(); // should not crash
    Thread::sleep_ms(150);
    t.stop().unwrap();
    assert!(PERIODIC_COUNT.load(Ordering::SeqCst) >= 2);
}

fn test_raii_drop() {
    PERIODIC_COUNT.store(0, Ordering::SeqCst);
    {
        let t = Timer::new(periodic_cb, 20, false).unwrap();
        t.start().unwrap();
        Thread::sleep_ms(60);
        // Timer dropped — should stop and destroy cleanly
    }
}

fn test_fn_callback_trampoline() {
    // Verify the fn() → extern "C" trampoline works correctly
    static TRAMPOLINE_FLAG: AtomicI32 = AtomicI32::new(0);
    fn trampoline_test_cb() {
        TRAMPOLINE_FLAG.store(42, Ordering::Release);
    }

    TRAMPOLINE_FLAG.store(0, Ordering::SeqCst);
    let t = Timer::new(trampoline_test_cb, 20, true).unwrap();
    t.start().unwrap();
    Thread::sleep_ms(150);
    assert_eq!(TRAMPOLINE_FLAG.load(Ordering::SeqCst), 42);
}

pub fn run() -> (usize, usize) {
    run_suite("Timer", &[
        test_entry!(test_create_destroy_oneshot),
        test_entry!(test_create_destroy_periodic),
        test_entry!(test_oneshot_fires_once),
        test_entry!(test_periodic_fires_multiple),
        test_entry!(test_stop_prevents_callbacks),
        test_entry!(test_reset_restarts),
        test_entry!(test_double_start),
        test_entry!(test_raii_drop),
        test_entry!(test_fn_callback_trampoline),
    ])
}
