// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::{run_suite, PtrGuard};
use crate::test_entry;
use ove::{Queue, Thread, Error};
use std::sync::atomic::{AtomicPtr, AtomicI32, Ordering};

static Q_CONSUMER_SUM: AtomicI32 = AtomicI32::new(0);
static Q_PTR: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

fn consumer_thread() {
    let q_ptr = Q_PTR.load(Ordering::Acquire);
    if q_ptr.is_null() { return; }
    let q = unsafe { &*(q_ptr as *const Queue<i32, 10>) };
    loop {
        match q.try_recv_for(core::time::Duration::from_millis(200)) {
            Ok(val) => { Q_CONSUMER_SUM.fetch_add(val, Ordering::Relaxed); }
            Err(_) => break,
        }
    }
}

static Q_BLOCKING: AtomicI32 = AtomicI32::new(0);
static Q_BLOCK_PTR: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

fn blocking_receiver() {
    let q_ptr = Q_BLOCK_PTR.load(Ordering::Acquire);
    if q_ptr.is_null() { return; }
    let q = unsafe { &*(q_ptr as *const Queue<i32, 5>) };
    if let Ok(val) = q.recv() {
        Q_BLOCKING.store(val, Ordering::Release);
    }
}

fn test_create_destroy() {
    let _q = Queue::<i32, 5>::new().unwrap();
}

fn test_send_receive_single() {
    let q = Queue::<i32, 5>::new().unwrap();
    q.try_send(&42).unwrap();
    let val = q.try_recv().unwrap();
    assert_eq!(val, 42);
}

fn test_fifo_order() {
    let q = Queue::<i32, 10>::new().unwrap();
    for i in 0..5 {
        q.try_send(&i).unwrap();
    }
    for i in 0..5 {
        let val = q.try_recv().unwrap();
        assert_eq!(val, i);
    }
}

fn test_send_full_times_out() {
    let q = Queue::<i32, 2>::new().unwrap();
    q.try_send(&1).unwrap();
    q.try_send(&2).unwrap();
    let result = q.try_send_for(&3, core::time::Duration::from_millis(10));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_receive_empty_times_out() {
    let q = Queue::<i32, 5>::new().unwrap();
    let result = q.try_recv_for(core::time::Duration::from_millis(10));
    assert!(matches!(result, Err(Error::Timeout)));
}

fn test_send_from_isr() {
    let q = Queue::<i32, 5>::new().unwrap();
    q.send_from_isr(&99).unwrap();
    let val = q.try_recv().unwrap();
    assert_eq!(val, 99);
}

fn test_receive_from_isr() {
    let q = Queue::<i32, 5>::new().unwrap();
    q.try_send(&77).unwrap();
    let val = q.receive_from_isr().unwrap();
    assert_eq!(val, 77);
}

fn test_producer_consumer() {
    let q = Queue::<i32, 10>::new().unwrap();
    Q_CONSUMER_SUM.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&Q_PTR, &q as *const Queue<i32, 10> as *mut ());

    let th = Thread::builder().name(c"cons").priority(ove::Priority::Low).stack_size(4096).spawn_simple(consumer_thread).unwrap();

    for i in 1..=5 {
        q.try_send_for(&i, core::time::Duration::from_millis(100)).unwrap();
        Thread::sleep_ms(5);
    }

    Thread::sleep_ms(500);
    drop(th);

    drop(_guard);
    assert_eq!(Q_CONSUMER_SUM.load(Ordering::SeqCst), 15);
}

fn test_struct_item() {
    #[derive(Copy, Clone, PartialEq, Eq, Debug)]
    struct Pair { a: i32, b: i32 }

    let q = Queue::<Pair, 4>::new().unwrap();
    q.try_send(&Pair { a: 10, b: 20 }).unwrap();
    let out = q.try_recv().unwrap();
    assert_eq!(out, Pair { a: 10, b: 20 });
}

fn test_send_wait_forever() {
    let q = Queue::<i32, 5>::new().unwrap();
    Q_BLOCKING.store(0, Ordering::SeqCst);
    let _guard = PtrGuard::new(&Q_BLOCK_PTR, &q as *const Queue<i32, 5> as *mut ());

    let th = Thread::builder().name(c"blk").priority(ove::Priority::Low).stack_size(4096).spawn_simple(blocking_receiver).unwrap();
    Thread::sleep_ms(50);

    q.try_send(&123).unwrap();
    Thread::sleep_ms(100);
    assert_eq!(Q_BLOCKING.load(Ordering::SeqCst), 123);

    drop(th);
    drop(_guard);
}

fn test_raii_drop() {
    {
        let q = Queue::<i32, 5>::new().unwrap();
        q.try_send(&1).unwrap();
    }
}

fn test_type_safety() {
    let q8 = Queue::<u8, 4>::new().unwrap();
    q8.try_send(&0xAB).unwrap();
    let v8 = q8.try_recv().unwrap();
    assert_eq!(v8, 0xAB);

    let q32 = Queue::<u32, 4>::new().unwrap();
    q32.try_send(&0xDEADBEEF).unwrap();
    let v32 = q32.try_recv().unwrap();
    assert_eq!(v32, 0xDEADBEEF);
}

pub fn run() -> (usize, usize) {
    run_suite("Queue", &[
        test_entry!(test_create_destroy),
        test_entry!(test_send_receive_single),
        test_entry!(test_fifo_order),
        test_entry!(test_send_full_times_out),
        test_entry!(test_receive_empty_times_out),
        test_entry!(test_send_from_isr),
        test_entry!(test_receive_from_isr),
        test_entry!(test_producer_consumer),
        test_entry!(test_struct_item),
        test_entry!(test_send_wait_forever),
        test_entry!(test_raii_drop),
        test_entry!(test_type_safety),
    ])
}
