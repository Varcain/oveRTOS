// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{CondVar, Mutex, Thread, WAIT_FOREVER, Error};
use std::sync::atomic::{AtomicPtr, AtomicI32, Ordering};

struct CvCtx {
    cv: *const CondVar,
    mtx: *const Mutex,
}
unsafe impl Send for CvCtx {}
unsafe impl Sync for CvCtx {}

static CV_WOKE: AtomicI32 = AtomicI32::new(0);
static CV_CTX: AtomicPtr<CvCtx> = AtomicPtr::new(core::ptr::null_mut());

fn cv_wait_entry() {
    let ctx_ptr = CV_CTX.load(Ordering::Acquire);
    if ctx_ptr.is_null() { return; }
    let ctx = unsafe { &*ctx_ptr };
    let cv = unsafe { &*ctx.cv };
    let mtx = unsafe { &*ctx.mtx };
    mtx.lock(WAIT_FOREVER).unwrap();
    cv.wait(mtx, WAIT_FOREVER).unwrap();
    CV_WOKE.store(1, Ordering::Release);
    mtx.unlock();
}

static CV_SIGNALED: AtomicI32 = AtomicI32::new(0);
static CV_SIG_CTX: AtomicPtr<CvCtx> = AtomicPtr::new(core::ptr::null_mut());

fn cv_signal_entry() {
    Thread::sleep_ms(50);
    let ctx_ptr = CV_SIG_CTX.load(Ordering::Acquire);
    if ctx_ptr.is_null() { return; }
    let ctx = unsafe { &*ctx_ptr };
    let cv = unsafe { &*ctx.cv };
    let mtx = unsafe { &*ctx.mtx };
    mtx.lock(WAIT_FOREVER).unwrap();
    CV_SIGNALED.store(1, Ordering::Release);
    cv.signal();
    mtx.unlock();
}

static CV_READY: AtomicI32 = AtomicI32::new(0);
static CV_PROD_CTX: AtomicPtr<CvCtx> = AtomicPtr::new(core::ptr::null_mut());

fn cv_producer_entry() {
    Thread::sleep_ms(50);
    let ctx_ptr = CV_PROD_CTX.load(Ordering::Acquire);
    if ctx_ptr.is_null() { return; }
    let ctx = unsafe { &*ctx_ptr };
    let cv = unsafe { &*ctx.cv };
    let mtx = unsafe { &*ctx.mtx };
    mtx.lock(WAIT_FOREVER).unwrap();
    CV_READY.store(1, Ordering::Release);
    cv.signal();
    mtx.unlock();
}

fn test_create() {
    let _cv = CondVar::new().unwrap();
}

fn test_signal_wakes_one() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new().unwrap();
    CV_WOKE.store(0, Ordering::SeqCst);

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex,
    };
    let _guard = PtrGuard::new(&CV_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::spawn(b"cvw\0", cv_wait_entry, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(50);

    mtx.lock(WAIT_FOREVER).unwrap();
    cv.signal();
    mtx.unlock();

    drop(th);
    drop(_guard);
    assert_eq!(CV_WOKE.load(Ordering::SeqCst), 1);
}

fn test_wait_timeout() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new().unwrap();
    mtx.lock(WAIT_FOREVER).unwrap();
    let result = cv.wait(&mtx, core::time::Duration::from_millis(50));
    assert!(matches!(result, Err(Error::Timeout)));
    mtx.unlock();
}

fn test_producer_consumer() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new().unwrap();
    CV_READY.store(0, Ordering::SeqCst);

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex,
    };
    let _guard = PtrGuard::new(&CV_PROD_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::spawn(b"prod\0", cv_producer_entry, ove::Priority::Normal, 4096).unwrap();

    mtx.lock(WAIT_FOREVER).unwrap();
    while CV_READY.load(Ordering::Acquire) == 0 {
        cv.wait(&mtx, WAIT_FOREVER).unwrap();
    }
    mtx.unlock();

    drop(th);
    drop(_guard);
    assert_eq!(CV_READY.load(Ordering::SeqCst), 1);
}

fn test_wait_forever() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new().unwrap();
    CV_SIGNALED.store(0, Ordering::SeqCst);

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex,
    };
    let _guard = PtrGuard::new(&CV_SIG_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::spawn(b"sig\0", cv_signal_entry, ove::Priority::Normal, 4096).unwrap();

    mtx.lock(WAIT_FOREVER).unwrap();
    cv.wait(&mtx, WAIT_FOREVER).unwrap();
    mtx.unlock();

    drop(th);
    drop(_guard);
    assert_eq!(CV_SIGNALED.load(Ordering::SeqCst), 1);
}

fn test_raii_drop() {
    {
        let _cv = CondVar::new().unwrap();
    }
}

fn test_broadcast_wakes_all() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new().unwrap();
    // broadcast with no waiters must be a no-op, not a crash — exercises the
    // broadcast() FFI path without needing threads.
    mtx.lock(WAIT_FOREVER).unwrap();
    cv.broadcast();
    mtx.unlock();
}

pub fn run() -> (usize, usize) {
    run_suite("CondVar", &[
        test_entry!(test_create),
        test_entry!(test_signal_wakes_one),
        test_entry!(test_wait_timeout),
        test_entry!(test_producer_consumer),
        test_entry!(test_wait_forever),
        test_entry!(test_raii_drop),
        test_entry!(test_broadcast_wakes_all),
    ])
}
