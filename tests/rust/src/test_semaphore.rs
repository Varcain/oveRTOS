// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{Semaphore, Thread, Error};
use std::sync::atomic::{AtomicPtr, AtomicI32, Ordering};

static SEM_DONE: AtomicI32 = AtomicI32::new(0);
static SEM_PTR: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

fn sem_give_entry() {
    Thread::sleep_ms(50);
    let sem_ptr = SEM_PTR.load(Ordering::Acquire);
    if !sem_ptr.is_null() {
        let sem = unsafe { &*(sem_ptr as *const Semaphore) };
        sem.release();
    }
    SEM_DONE.store(1, Ordering::Release);
}

fn test_create_binary() {
    let _sem = Semaphore::new(1, 1).unwrap();
}

fn test_create_counting() {
    let _sem = Semaphore::new(0, 10).unwrap();
}

fn test_take_initial_one() {
    let sem = Semaphore::new(1, 1).unwrap();
    sem.try_acquire().unwrap();
}

fn test_take_timeout() {
    let sem = Semaphore::new(0, 10).unwrap();
    let result = sem.try_acquire_for(core::time::Duration::from_millis(50));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_give_then_take() {
    let sem = Semaphore::new(0, 10).unwrap();
    sem.release();
    sem.try_acquire().unwrap();
}

fn test_counting() {
    let sem = Semaphore::new(0, 10).unwrap();
    for _ in 0..3 {
        sem.release();
    }
    for _ in 0..3 {
        sem.try_acquire().unwrap();
    }
    let result = sem.try_acquire_for(core::time::Duration::from_millis(10));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_producer_consumer() {
    let sem = Semaphore::new(0, 1).unwrap();
    SEM_DONE.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&SEM_PTR, &sem as *const Semaphore as *mut ());

    let th = Thread::builder().name(c"prod").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(sem_give_entry).unwrap();
    sem.try_acquire_for(core::time::Duration::from_millis(500)).unwrap();
    drop(th);

    drop(_guard);
    assert_eq!(SEM_DONE.load(Ordering::SeqCst), 1);
}

fn test_raii_drop() {
    {
        let sem = Semaphore::new(1, 1).unwrap();
        sem.try_acquire().unwrap();
        sem.release();
    }
}

pub fn run() -> (usize, usize) {
    run_suite("Semaphore", &[
        test_entry!(test_create_binary),
        test_entry!(test_create_counting),
        test_entry!(test_take_initial_one),
        test_entry!(test_take_timeout),
        test_entry!(test_give_then_take),
        test_entry!(test_counting),
        test_entry!(test_producer_consumer),
        test_entry!(test_raii_drop),
    ])
}
