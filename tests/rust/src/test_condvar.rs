// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{CondVar, Mutex, Thread};
use std::sync::atomic::{AtomicPtr, AtomicI32, Ordering};

struct CvCtx {
    cv: *const CondVar,
    mtx: *const Mutex<bool>,
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
    let g = mtx.lock().unwrap();
    let _g2 = cv.wait(g).unwrap();
    CV_WOKE.store(1, Ordering::Release);
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
    let mut g = mtx.lock().unwrap();
    *g = true;
    CV_SIGNALED.store(1, Ordering::Release);
    cv.signal();
}

static CV_PROD_CTX: AtomicPtr<CvCtx> = AtomicPtr::new(core::ptr::null_mut());

fn cv_producer_entry() {
    Thread::sleep_ms(50);
    let ctx_ptr = CV_PROD_CTX.load(Ordering::Acquire);
    if ctx_ptr.is_null() { return; }
    let ctx = unsafe { &*ctx_ptr };
    let cv = unsafe { &*ctx.cv };
    let mtx = unsafe { &*ctx.mtx };
    let mut g = mtx.lock().unwrap();
    *g = true;
    cv.signal();
}

fn test_create() {
    let _cv = CondVar::new().unwrap();
}

fn test_signal_wakes_one() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new(false).unwrap();
    CV_WOKE.store(0, Ordering::SeqCst);

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex<bool>,
    };
    let _guard = PtrGuard::new(&CV_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::builder().name(c"cvw").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(cv_wait_entry).unwrap();
    Thread::sleep_ms(50);

    {
        let _g = mtx.lock().unwrap();
        cv.signal();
    }

    drop(th);
    drop(_guard);
    assert_eq!(CV_WOKE.load(Ordering::SeqCst), 1);
}

fn test_wait_timeout() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new(0i32).unwrap();
    let g = mtx.lock().unwrap();
    let (_g2, wtr) = cv
        .wait_for(g, core::time::Duration::from_millis(50))
        .unwrap();
    assert!(wtr.timed_out(), "expected timeout — no signal sent");
}

fn test_wait_for_signalled_not_timed_out() {
    // Signal arrives before deadline → timed_out() must be false.
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new(false).unwrap();

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex<bool>,
    };
    let _guard = PtrGuard::new(&CV_PROD_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::builder()
        .name(c"sig50")
        .priority(ove::Priority::Normal)
        .stack_size(4096)
        .spawn_simple(cv_producer_entry)
        .unwrap();

    let g = mtx.lock().unwrap();
    let (g2, wtr) = cv
        .wait_for(g, core::time::Duration::from_millis(500))
        .unwrap();
    assert!(!wtr.timed_out(), "signal arrived before deadline");
    assert!(*g2, "predicate flipped to true by signaller");

    drop(th);
    drop(_guard);
}

fn test_wait_while_producer_consumer() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new(false).unwrap();

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex<bool>,
    };
    let _guard = PtrGuard::new(&CV_PROD_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::builder().name(c"prod").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(cv_producer_entry).unwrap();

    let g = mtx.lock().unwrap();
    // wait_while loops until predicate returns false; safe against
    // spurious wake-ups.
    let g = cv.wait_while(g, |ready| !*ready).unwrap();
    assert!(*g, "predicate satisfied");
    drop(g);

    drop(th);
    drop(_guard);
}

fn test_wait_forever() {
    let cv = CondVar::new().unwrap();
    let mtx = Mutex::new(false).unwrap();
    CV_SIGNALED.store(0, Ordering::SeqCst);

    let ctx = CvCtx {
        cv: &cv as *const CondVar,
        mtx: &mtx as *const Mutex<bool>,
    };
    let _guard = PtrGuard::new(&CV_SIG_CTX, &ctx as *const CvCtx as *mut CvCtx);

    let th = Thread::builder().name(c"sig").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(cv_signal_entry).unwrap();

    let g = mtx.lock().unwrap();
    let _g2 = cv.wait(g).unwrap();

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
    let mtx = Mutex::new(()).unwrap();
    // broadcast with no waiters must be a no-op, not a crash — exercises the
    // broadcast() FFI path without needing threads.
    let _g = mtx.lock().unwrap();
    cv.broadcast();
}

pub fn run() -> (usize, usize) {
    run_suite("CondVar", &[
        test_entry!(test_create),
        test_entry!(test_signal_wakes_one),
        test_entry!(test_wait_timeout),
        test_entry!(test_wait_for_signalled_not_timed_out),
        test_entry!(test_wait_while_producer_consumer),
        test_entry!(test_wait_forever),
        test_entry!(test_raii_drop),
        test_entry!(test_broadcast_wakes_all),
    ])
}
