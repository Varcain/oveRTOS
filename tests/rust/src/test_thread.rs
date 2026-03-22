// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Thread, ThreadState, Error};
use std::sync::atomic::{AtomicI32, Ordering};

static FLAG: AtomicI32 = AtomicI32::new(0);
static KEEP_RUNNING: AtomicI32 = AtomicI32::new(0);

fn entry_set_flag() {
    FLAG.store(1, Ordering::Release);
}

fn entry_spin() {
    while KEEP_RUNNING.load(Ordering::Acquire) != 0 {
        Thread::sleep_ms(1);
    }
}

fn entry_sleep_briefly() {
    FLAG.store(1, Ordering::Release);
    Thread::sleep_ms(200);
    FLAG.store(2, Ordering::Release);
}

fn test_create_destroy() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::spawn(b"t1\0", entry_set_flag, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(50);
    assert_eq!(FLAG.load(Ordering::SeqCst), 1);
    drop(th);
}

fn test_sleep_duration() {
    let before = std::time::Instant::now();
    Thread::sleep_ms(50);
    let elapsed = before.elapsed().as_millis();
    assert!(elapsed >= 25 && elapsed <= 150, "elapsed={}ms", elapsed);
}

fn test_yield_no_crash() {
    Thread::yield_now();
}

fn test_get_self() {
    let _t = Thread::current();
}

fn test_set_priority() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t7\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(10);
    th.set_priority(ove::Priority::High);
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_get_state_running() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t8\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(20);
    let st = th.get_state();
    assert!(
        st == ThreadState::Running || st == ThreadState::Ready || st == ThreadState::Blocked,
        "state={:?}",
        st
    );
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_get_state_terminated() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::spawn(b"t9\0", entry_set_flag, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(100);
    let st = th.get_state();
    assert!(
        st == ThreadState::Terminated || st == ThreadState::Suspended,
        "state={:?}",
        st
    );
    drop(th);
}

fn test_stack_usage() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t10\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(10);
    let _usage = th.get_stack_usage();
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_suspend_resume() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::spawn(b"t14\0", entry_sleep_briefly, ove::Priority::Normal, 4096).unwrap();
    for _ in 0..100 {
        if FLAG.load(Ordering::Acquire) != 0 { break; }
        Thread::sleep_ms(5);
    }
    assert_eq!(FLAG.load(Ordering::SeqCst), 1);

    th.suspend();
    Thread::sleep_ms(10);
    th.resume();
    Thread::sleep_ms(300);
    drop(th);
}

fn test_runtime_stats() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t16\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(20);
    let result = th.get_runtime_stats();
    assert!(result.is_ok() || matches!(result, Err(Error::NotSupported)));
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_raii_drop() {
    FLAG.store(0, Ordering::SeqCst);
    {
        let _th = Thread::spawn(b"raii\0", entry_set_flag, ove::Priority::Normal, 4096).unwrap();
        Thread::sleep_ms(50);
    }
}

pub fn run() -> (usize, usize) {
    run_suite("Thread", &[
        test_entry!(test_create_destroy),
        test_entry!(test_sleep_duration),
        test_entry!(test_yield_no_crash),
        test_entry!(test_get_self),
        test_entry!(test_set_priority),
        test_entry!(test_get_state_running),
        test_entry!(test_get_state_terminated),
        test_entry!(test_stack_usage),
        test_entry!(test_suspend_resume),
        test_entry!(test_runtime_stats),
        test_entry!(test_raii_drop),
    ])
}
