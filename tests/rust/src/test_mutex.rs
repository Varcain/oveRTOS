// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{PtrGuard, run_suite};
use crate::test_entry;
use ove::{Error, Mutex, MutexAccess, Thread};
use static_assertions::assert_not_impl_all;
use std::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

// A data view cannot move to another thread or perform a consuming wait there.
assert_not_impl_all!(MutexAccess<'static, ()>: Send);

static COUNTER: AtomicI32 = AtomicI32::new(0);
static COUNTER_MTX: AtomicPtr<Mutex<()>> = AtomicPtr::new(core::ptr::null_mut());

fn counter_entry() {
    let pointer = COUNTER_MTX.load(Ordering::Acquire);
    if pointer.is_null() {
        return;
    }
    let mutex = unsafe { &*pointer };
    for _ in 0..1000 {
        mutex
            .with_lock(|_| {
                COUNTER.fetch_add(1, Ordering::Relaxed);
                Ok(())
            })
            .unwrap();
    }
}

fn test_create() {
    let mtx = Mutex::new(()).unwrap();
    drop(mtx);
}

fn test_lock_unlock() {
    let mutex = Mutex::new(()).unwrap();
    mutex.with_lock(|_| Ok(())).unwrap();
}

static HOLD_MTX: AtomicPtr<Mutex<()>> = AtomicPtr::new(core::ptr::null_mut());
static HOLD_LOCKED: AtomicI32 = AtomicI32::new(0);
static HOLD_RELEASE: AtomicI32 = AtomicI32::new(0);

fn hold_lock_entry() {
    let pointer = HOLD_MTX.load(Ordering::Acquire);
    if pointer.is_null() {
        return;
    }
    let mutex = unsafe { &*pointer };
    mutex
        .with_lock(|_| {
            HOLD_LOCKED.store(1, Ordering::Release);
            while HOLD_RELEASE.load(Ordering::Acquire) == 0 {
                Thread::sleep_ms(1);
            }
            Ok(())
        })
        .unwrap();
}

fn test_contention_timeout() {
    let mtx = Mutex::new(()).unwrap();
    HOLD_LOCKED.store(0, Ordering::SeqCst);
    HOLD_RELEASE.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&HOLD_MTX, &mtx as *const Mutex<()> as *mut Mutex<()>);

    // Spawn thread that holds the lock
    let th = Thread::builder()
        .name(c"hold")
        .priority(ove::Priority::Normal)
        .stack_size(4096)
        .spawn_simple(hold_lock_entry)
        .unwrap();

    // Wait until the thread holds the lock
    for _ in 0..200 {
        if HOLD_LOCKED.load(Ordering::Acquire) != 0 {
            break;
        }
        Thread::sleep_ms(1);
    }
    assert_eq!(HOLD_LOCKED.load(Ordering::SeqCst), 1);

    // Now try to lock with timeout — should fail
    let result = mtx.try_with_lock_for(core::time::Duration::from_millis(50), |_| Ok(()));
    assert!(matches!(result, Err(Error::Timeout)), "expected timeout");

    // Release the holder
    HOLD_RELEASE.store(1, Ordering::Release);
    drop(th);
    drop(_guard);
}

fn test_lock_zero_timeout() {
    let mutex = Mutex::new(()).unwrap();
    mutex.try_with_lock(|_| Ok(())).unwrap();
}

fn test_raii_drop() {
    let mutex = Mutex::new(()).unwrap();
    mutex.with_lock(|_| Ok(())).unwrap();
}

fn test_scope_auto_unlock() {
    let mutex = Mutex::new(()).unwrap();
    mutex.with_lock(|_| Ok(())).unwrap();
    mutex.try_with_lock(|_| Ok(())).unwrap();
}

fn test_scope_contention() {
    let mutex = Mutex::new(()).unwrap();
    mutex
        .with_lock(|_| {
            let result = mutex.try_with_lock(|_| Ok(()));
            assert!(matches!(result, Err(Error::Timeout | Error::Busy)));
            Ok(())
        })
        .unwrap();
}

fn test_error_mapping() {
    let mutex = Mutex::new(()).unwrap();
    mutex
        .with_lock(|_| {
            let result = mutex.try_with_lock(|_| Ok(()));
            assert!(matches!(result, Err(Error::Timeout | Error::Busy)));
            Ok(())
        })
        .unwrap();
}

fn test_shared_counter() {
    let mtx = Mutex::new(()).unwrap();
    COUNTER.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&COUNTER_MTX, &mtx as *const Mutex<()> as *mut Mutex<()>);

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

fn test_access_debug_format() {
    let mutex = Mutex::new(()).unwrap();
    let text = mutex.with_lock(|access| Ok(format!("{access:?}"))).unwrap();
    assert!(text.contains("MutexAccess"), "unexpected debug: {text}");
    assert!(text.contains("data"), "unexpected debug: {text}");
}

fn test_mutex_debug_format() {
    let mtx = Mutex::new(()).unwrap();
    // `Mutex<T>` has a hand-rolled Debug impl that does not lock (would
    // deadlock if T: Debug had side-effects).  Show only the handle.
    let s = format!("{:?}", mtx);
    assert!(s.contains("Mutex"), "unexpected debug: {s}");
    assert!(s.contains("handle"), "unexpected debug: {s}");
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Mutex",
        &[
            test_entry!(test_create),
            test_entry!(test_lock_unlock),
            test_entry!(test_contention_timeout),
            test_entry!(test_lock_zero_timeout),
            test_entry!(test_raii_drop),
            test_entry!(test_scope_auto_unlock),
            test_entry!(test_scope_contention),
            test_entry!(test_error_mapping),
            test_entry!(test_shared_counter),
            test_entry!(test_access_debug_format),
            test_entry!(test_mutex_debug_format),
        ],
    )
}
