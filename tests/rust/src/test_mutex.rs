// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{Mutex, MutexGuard, Thread, WAIT_FOREVER, Error};
use static_assertions::assert_not_impl_all;
use std::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

// Compile-time invariant: a MutexGuard cannot be sent to another thread.
// The locking thread must release the lock; cross-thread Send would
// allow `ove_mutex_unlock` from a thread that never issued the matching
// `ove_mutex_lock` — backend-defined UB (POSIX EPERM, FreeRTOS assert).
assert_not_impl_all!(MutexGuard<'static>: Send);

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
    let th = Thread::builder().name(c"hold").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(hold_lock_entry).unwrap();

    // Wait until the thread holds the lock
    for _ in 0..200 {
        if HOLD_LOCKED.load(Ordering::Acquire) != 0 { break; }
        Thread::sleep_ms(1);
    }
    assert_eq!(HOLD_LOCKED.load(Ordering::SeqCst), 1);

    // Now try to lock with timeout — should fail
    let result = mtx.lock(core::time::Duration::from_millis(50));
    assert!(matches!(result, Err(Error::Timeout)), "expected timeout, got {:?}", result);

    // Release the holder
    HOLD_RELEASE.store(1, Ordering::Release);
    drop(th);
    drop(_guard);
}

fn test_lock_zero_timeout() {
    let mtx = Mutex::new().unwrap();
    mtx.lock(core::time::Duration::ZERO).unwrap();
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
    mtx.lock(core::time::Duration::ZERO).unwrap();
    mtx.unlock();
}

fn test_guard_timeout() {
    let mtx = Mutex::new().unwrap();
    let _guard = mtx.guard(WAIT_FOREVER).unwrap();
    // Try to acquire again — should timeout
    let result = mtx.lock(core::time::Duration::ZERO);
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_error_mapping() {
    let mtx = Mutex::new().unwrap();
    mtx.lock(WAIT_FOREVER).unwrap();
    // Try-lock on already-held (same thread, non-recursive) returns timeout with 0ms
    let result = mtx.lock(core::time::Duration::ZERO);
    assert!(matches!(result, Err(Error::Timeout)));
    mtx.unlock();
}

fn test_shared_counter() {
    let mtx = Mutex::new().unwrap();
    COUNTER.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&COUNTER_MTX, &mtx as *const Mutex as *mut Mutex);

    let t1 = Thread::builder()
        .name(c"c1")
        .priority(ove::Priority::Normal)
        .stack_size(4096)
        .spawn_simple(counter_entry)
        .unwrap();
    let t2 = Thread::builder()
        .name(c"c2")
        .priority(ove::Priority::Normal)
        .stack_size(4096)
        .spawn_simple(counter_entry)
        .unwrap();
    drop(t1);
    drop(t2);

    drop(_guard);
    assert_eq!(COUNTER.load(Ordering::SeqCst), 2000);
}

fn test_guard_debug_format() {
    let mtx = Mutex::new().unwrap();
    let guard = mtx.guard(WAIT_FOREVER).unwrap();
    let s = format!("{:?}", guard);
    assert!(s.contains("MutexGuard"), "unexpected debug: {s}");
    assert!(s.contains("mutex"), "unexpected debug: {s}");
}

fn test_mutex_debug_format() {
    let mtx = Mutex::new().unwrap();
    // Exercises the `@debug` arm of `ove_handle_impl!` — the macro-generated
    // `impl Debug for Mutex` is the only way those lines get hit.
    let s = format!("{:?}", mtx);
    assert!(s.contains("Mutex"), "unexpected debug: {s}");
    assert!(s.contains("handle"), "unexpected debug: {s}");
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
        test_entry!(test_guard_debug_format),
        test_entry!(test_mutex_debug_format),
    ])
}
