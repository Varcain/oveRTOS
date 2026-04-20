// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// Error-path coverage. The rest of the suite exercises happy paths
// (`.unwrap()` on every Result) which makes passing the tests weaker
// evidence than it should be — a regression that turns a legitimate
// error into a panic would still read as "pass" in the unwrap sites.
// These tests pin down the specific `Error::*` variant returned by
// each common failure mode so the binding surface can't silently
// drop error mapping.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Error, EventGroup, Mutex, Queue, Semaphore, WaitFlags};

fn test_mutex_try_lock_contended_returns_timeout() {
    let mtx = Mutex::new().unwrap();
    mtx.lock(0).unwrap();
    let rc = mtx.lock(0);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
    mtx.unlock();
}

fn test_mutex_guard_contention_returns_timeout() {
    let mtx = Mutex::new().unwrap();
    let _first = mtx.guard(0).unwrap();
    let rc = mtx.guard(0);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
}

fn test_semaphore_take_empty_returns_timeout() {
    // Semaphore::new(initial, max) — start empty so take(0) times out.
    let sem = Semaphore::new(0, 1).unwrap();
    let rc = sem.take(0);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
}

fn test_queue_receive_empty_returns_timeout() {
    let q: Queue<u32, 4> = Queue::new().unwrap();
    let rc = q.receive(0);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
}

fn test_queue_send_full_returns_timeout() {
    let q: Queue<u32, 1> = Queue::new().unwrap();
    q.send(&42, 0).unwrap();
    let rc = q.send(&43, 0);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
    assert_eq!(q.receive(0).unwrap(), 42);
}

fn test_eventgroup_wait_bits_timeout() {
    let eg = EventGroup::new().unwrap();
    // WaitFlags::NONE → "wake on any bit set" (WAIT_ALL absent).
    let rc = eg.wait_bits(0x1, WaitFlags::NONE, 0);
    assert!(matches!(rc, Err(Error::Timeout)), "got {:?}", rc);
}

pub fn run() -> (usize, usize) {
    run_suite("Errors", &[
        test_entry!(test_mutex_try_lock_contended_returns_timeout),
        test_entry!(test_mutex_guard_contention_returns_timeout),
        test_entry!(test_semaphore_take_empty_returns_timeout),
        test_entry!(test_queue_receive_empty_returns_timeout),
        test_entry!(test_queue_send_full_returns_timeout),
        test_entry!(test_eventgroup_wait_bits_timeout),
    ])
}
