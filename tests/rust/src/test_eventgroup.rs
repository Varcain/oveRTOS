// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{EventGroup, Thread, Error, WaitFlags};
use std::sync::atomic::{AtomicPtr, Ordering};

const BIT_0: u32 = 1 << 0;
const BIT_1: u32 = 1 << 1;
const BIT_2: u32 = 1 << 2;

static EG_PTR: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

fn setter_thread() {
    Thread::sleep_ms(50);
    let eg_ptr = EG_PTR.load(Ordering::Acquire);
    if !eg_ptr.is_null() {
        let eg = unsafe { &*(eg_ptr as *const EventGroup) };
        eg.set_bits(BIT_0 | BIT_1);
    }
}

fn test_create_destroy() {
    let _eg = EventGroup::new().unwrap();
}

fn test_set_bits() {
    let eg = EventGroup::new().unwrap();
    eg.set_bits(BIT_0 | BIT_1);
    let bits = eg.get_bits();
    assert!(bits & BIT_0 != 0);
    assert!(bits & BIT_1 != 0);
}

fn test_clear_bits() {
    let eg = EventGroup::new().unwrap();
    eg.set_bits(BIT_0 | BIT_1 | BIT_2);
    eg.clear_bits(BIT_1);
    let remaining = eg.get_bits();
    assert!(remaining & BIT_0 != 0);
    assert!(remaining & BIT_1 == 0);
    assert!(remaining & BIT_2 != 0);
}

fn test_get_bits() {
    let eg = EventGroup::new().unwrap();
    assert_eq!(eg.get_bits(), 0);
    eg.set_bits(BIT_2);
    assert!(eg.get_bits() & BIT_2 != 0);
}

fn test_wait_all() {
    let eg = EventGroup::new().unwrap();
    eg.set_bits(BIT_0 | BIT_1);
    let actual = eg.wait_bits(BIT_0 | BIT_1, WaitFlags::WAIT_ALL, core::time::Duration::from_millis(100)).unwrap();
    assert_eq!(actual & (BIT_0 | BIT_1), BIT_0 | BIT_1);
}

fn test_wait_any() {
    let eg = EventGroup::new().unwrap();
    eg.set_bits(BIT_0);
    let actual = eg.wait_bits(BIT_0 | BIT_1, WaitFlags::NONE, core::time::Duration::from_millis(100)).unwrap();
    assert!(actual & BIT_0 != 0);
}

fn test_wait_timeout() {
    let eg = EventGroup::new().unwrap();
    let result = eg.wait_bits(BIT_0, WaitFlags::WAIT_ALL, core::time::Duration::from_millis(10));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_clear_on_exit() {
    let eg = EventGroup::new().unwrap();
    eg.set_bits(BIT_0 | BIT_1);
    let _actual = eg.wait_bits(BIT_0 | BIT_1, WaitFlags::WAIT_ALL | WaitFlags::CLEAR_ON_EXIT, core::time::Duration::from_millis(100)).unwrap();
    let remaining = eg.get_bits();
    assert!(remaining & BIT_0 == 0);
    assert!(remaining & BIT_1 == 0);
}

fn test_set_bits_from_isr() {
    let eg = EventGroup::new().unwrap();
    eg.set_bits_from_isr(BIT_2);
    assert!(eg.get_bits() & BIT_2 != 0);
}

fn test_cross_thread() {
    let eg = EventGroup::new().unwrap();
    EG_PTR.store(&eg as *const EventGroup as *mut (), Ordering::Release);

    let th = Thread::builder().name(c"set").priority(ove::Priority::Low).stack_size(4096).spawn_simple(setter_thread).unwrap();
    let actual = eg.wait_bits(BIT_0 | BIT_1, WaitFlags::WAIT_ALL, core::time::Duration::from_millis(500)).unwrap();
    assert_eq!(actual & (BIT_0 | BIT_1), BIT_0 | BIT_1);

    drop(th);
    EG_PTR.store(core::ptr::null_mut(), Ordering::Release);
}

fn test_raii_drop() {
    {
        let eg = EventGroup::new().unwrap();
        eg.set_bits(BIT_0);
    }
}

pub fn run() -> (usize, usize) {
    run_suite("EventGroup", &[
        test_entry!(test_create_destroy),
        test_entry!(test_set_bits),
        test_entry!(test_clear_bits),
        test_entry!(test_get_bits),
        test_entry!(test_wait_all),
        test_entry!(test_wait_any),
        test_entry!(test_wait_timeout),
        test_entry!(test_clear_on_exit),
        test_entry!(test_set_bits_from_isr),
        test_entry!(test_cross_thread),
        test_entry!(test_raii_drop),
    ])
}
