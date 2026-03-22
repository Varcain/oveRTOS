// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{Mutex, Thread, WAIT_FOREVER, Error};
use std::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

static COUNTER: AtomicI32 = AtomicI32::new(0);
static COUNTER_MTX: AtomicPtr<Mutex> = AtomicPtr::new(core::ptr::null_mut());

fn counter_entry() {
    let mtx_ptr = COUNTER_MTX.load(Ordering::Acquire);
    if mtx_ptr.is_null() { return; }
    let mtx = unsafe { &*(mtx_ptr as *const Mutex) };
    for _ in 0..1000 {
        mtx.lock(WAIT_FOREVER).unwrap();
        COUNTER.fetch_add(1, Ordering::Relaxed);
        mtx.unlock();
    }
}

fn test_create() {
    let mtx = Mutex::new().unwrap();
    drop(mtx);
}

fn test_lock_unlock() {
    let mtx = Mutex::new().unwrap();
    mtx.lock(WAIT_FOREVER).unwrap();
    mtx.unlock();
}

static HOLD_MTX: AtomicPtr<Mutex> = AtomicPtr::new(core::ptr::null_mut());
static HOLD_LOCKED: AtomicI32 = AtomicI32::new(0);
static HOLD_RELEASE: AtomicI32 = AtomicI32::new(0);

fn hold_lock_entry() {
    let mtx_ptr = HOLD_MTX.load(Ordering::Acquire);
    if mtx_ptr.is_null() { return; }
    let mtx = unsafe { &*(mtx_ptr as *const Mutex) };
    mtx.lock(WAIT_FOREVER).unwrap();
    HOLD_LOCKED.store(1, Ordering::Release);
    // Hold until told to release
    while HOLD_RELEASE.load(Ordering::Acquire) == 0 {
        Thread::sleep_ms(1);
    }
    mtx.unlock();
}

fn test_contention_timeout() {
    let mtx = Mutex::new().unwrap();
    HOLD_LOCKED.store(0, Ordering::SeqCst);
    HOLD_RELEASE.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&HOLD_MTX, &mtx as *const Mutex as *mut Mutex);

    // Spawn thread that holds the lock
    let th = Thread::spawn(b"hold\0", hold_lock_entry, ove::Priority::Normal, 4096).unwrap();

    // Wait until the thread holds the lock
    for _ in 0..200 {
        if HOLD_LOCKED.load(Ordering::Acquire) != 0 { break; }
        Thread::sleep_ms(1);
    }
    assert_eq!(HOLD_LOCKED.load(Ordering::SeqCst), 1);

    // Now try to lock with timeout — should fail
    let result = mtx.lock(50);
    assert!(matches!(result, Err(Error::Timeout)), "expected timeout, got {:?}", result);

    // Release the holder
    HOLD_RELEASE.store(1, Ordering::Release);
    drop(th);
    drop(_guard);
}

fn test_lock_zero_timeout() {
    let mtx = Mutex::new().unwrap();
    mtx.lock(0).unwrap();
    mtx.unlock();
}

fn test_raii_drop() {
    {
        let mtx = Mutex::new().unwrap();
        mtx.lock(WAIT_FOREVER).unwrap();
        mtx.unlock();
        // mtx dropped here — should not leak
    }
}

fn test_guard_auto_unlock() {
    let mtx = Mutex::new().unwrap();
    {
        let _guard = mtx.guard(WAIT_FOREVER).unwrap();
        // Guard holds the lock
    }
    // Guard dropped — mutex should be unlocked
    mtx.lock(0).unwrap();
    mtx.unlock();
}

fn test_guard_timeout() {
    let mtx = Mutex::new().unwrap();
    let _guard = mtx.guard(WAIT_FOREVER).unwrap();
    // Try to acquire again — should timeout
    let result = mtx.lock(0);
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_error_mapping() {
    let mtx = Mutex::new().unwrap();
    mtx.lock(WAIT_FOREVER).unwrap();
    // Try-lock on already-held (same thread, non-recursive) returns timeout with 0ms
    let result = mtx.lock(0);
    assert!(matches!(result, Err(Error::Timeout)));
    mtx.unlock();
}

fn test_shared_counter() {
    let mtx = Mutex::new().unwrap();
    COUNTER.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&COUNTER_MTX, &mtx as *const Mutex as *mut Mutex);

    let name1 = b"c1\0";
    let name2 = b"c2\0";
    let t1 = Thread::spawn(name1, counter_entry, ove::Priority::Normal, 4096).unwrap();
    let t2 = Thread::spawn(name2, counter_entry, ove::Priority::Normal, 4096).unwrap();
    drop(t1);
    drop(t2);

    drop(_guard);
    assert_eq!(COUNTER.load(Ordering::SeqCst), 2000);
}

pub fn run() -> (usize, usize) {
    run_suite("Mutex", &[
        test_entry!(test_create),
        test_entry!(test_lock_unlock),
        test_entry!(test_contention_timeout),
        test_entry!(test_lock_zero_timeout),
        test_entry!(test_raii_drop),
        test_entry!(test_guard_auto_unlock),
        test_entry!(test_guard_timeout),
        test_entry!(test_error_mapping),
        test_entry!(test_shared_counter),
    ])
}
