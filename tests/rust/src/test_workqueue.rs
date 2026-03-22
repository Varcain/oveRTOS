// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Workqueue, Work, Thread, Priority};
use std::sync::atomic::{AtomicI32, Ordering};

static WQ_COUNT: AtomicI32 = AtomicI32::new(0);

unsafe extern "C" fn test_work_handler(_work: ove::ffi::ove_work_t) {
    WQ_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn test_workqueue_create_destroy() {
    let _wq = Workqueue::new(b"test\0", Priority::Normal, 4096).unwrap();
}

fn test_work_create_destroy() {
    let _w = Work::new(Some(test_work_handler)).unwrap();
}

fn test_work_submit() {
    WQ_COUNT.store(0, Ordering::SeqCst);
    let wq = Workqueue::new(b"sub\0", Priority::Normal, 4096).unwrap();
    let w = Work::new(Some(test_work_handler)).unwrap();
    w.submit(&wq).unwrap();
    Thread::sleep_ms(100);
    assert_eq!(WQ_COUNT.load(Ordering::SeqCst), 1);
}

fn test_work_submit_delayed() {
    WQ_COUNT.store(0, Ordering::SeqCst);
    let wq = Workqueue::new(b"del\0", Priority::Normal, 4096).unwrap();
    let w = Work::new(Some(test_work_handler)).unwrap();
    w.submit_delayed(&wq, 50).unwrap();

    Thread::sleep_ms(10);
    assert_eq!(WQ_COUNT.load(Ordering::SeqCst), 0);

    Thread::sleep_ms(150);
    assert_eq!(WQ_COUNT.load(Ordering::SeqCst), 1);
}

fn test_work_cancel() {
    let wq = Workqueue::new(b"can\0", Priority::Normal, 4096).unwrap();
    let w = Work::new(Some(test_work_handler)).unwrap();
    let _ = w.cancel(); /* best-effort */
    drop(w);
    drop(wq);
}

fn test_raii_drop() {
    {
        let _wq = Workqueue::new(b"raii\0", Priority::Normal, 4096).unwrap();
    }
    {
        let _w = Work::new(Some(test_work_handler)).unwrap();
    }
}

pub fn run() -> (usize, usize) {
    run_suite("Workqueue", &[
        test_entry!(test_workqueue_create_destroy),
        test_entry!(test_work_create_destroy),
        test_entry!(test_work_submit),
        test_entry!(test_work_submit_delayed),
        test_entry!(test_work_cancel),
        test_entry!(test_raii_drop),
    ])
}
