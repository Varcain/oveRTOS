// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{Event, Thread, Error};
use std::sync::atomic::{AtomicPtr, AtomicI32, Ordering};

static EVT_DONE: AtomicI32 = AtomicI32::new(0);
static EVT_PTR: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

fn evt_signal_entry() {
    Thread::sleep_ms(50);
    let evt_ptr = EVT_PTR.load(Ordering::Acquire);
    if !evt_ptr.is_null() {
        let evt = unsafe { &*(evt_ptr as *const Event) };
        evt.signal();
    }
    EVT_DONE.store(1, Ordering::Release);
}

fn test_create() {
    let _evt = Event::new().unwrap();
}

fn test_signal_then_wait() {
    let evt = Event::new().unwrap();
    evt.signal();
    evt.wait(core::time::Duration::ZERO).unwrap();
}

fn test_wait_timeout() {
    let evt = Event::new().unwrap();
    let result = evt.wait(core::time::Duration::from_millis(50));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_cross_thread() {
    let evt = Event::new().unwrap();
    EVT_DONE.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&EVT_PTR, &evt as *const Event as *mut ());

    let th = Thread::spawn(b"esig\0", evt_signal_entry, ove::Priority::Normal, 4096).unwrap();
    evt.wait(core::time::Duration::from_millis(500)).unwrap();
    drop(th);

    drop(_guard);
    assert_eq!(EVT_DONE.load(Ordering::SeqCst), 1);
}

fn test_signal_from_isr() {
    let evt = Event::new().unwrap();
    evt.signal_from_isr();
    evt.wait(core::time::Duration::ZERO).unwrap();
}

fn test_auto_reset() {
    let evt = Event::new().unwrap();
    evt.signal();
    evt.wait(core::time::Duration::ZERO).unwrap();
    let result = evt.wait(core::time::Duration::from_millis(50));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_raii_drop() {
    {
        let evt = Event::new().unwrap();
        evt.signal();
        evt.wait(core::time::Duration::ZERO).unwrap();
    }
}

pub fn run() -> (usize, usize) {
    run_suite("Event", &[
        test_entry!(test_create),
        test_entry!(test_signal_then_wait),
        test_entry!(test_wait_timeout),
        test_entry!(test_cross_thread),
        test_entry!(test_signal_from_isr),
        test_entry!(test_auto_reset),
        test_entry!(test_raii_drop),
    ])
}
