// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Zero-heap (static-storage) construction suite — only compiled under
//! `#[cfg(zero_heap)]`.
//!
//! Exercises the binding's public `*Storage` + `create(&STORAGE, …)`
//! constructors (the static-storage path the heap `::new()` suites can't
//! reach).  Each primitive is built from a `static` storage cell and put
//! through one basic operation, proving the zero-heap `from_static` plumbing
//! links and runs against the zero-heap stub.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{
    CondVar, CondVarStorage, Event, EventGroup, EventGroupStorage, EventStorage, Mutex,
    MutexStorage, Queue, QueueStorage, RecursiveMutex, RecursiveMutexStorage, Semaphore,
    SemaphoreStorage, Stream, StreamStorage, Timer, TimerStorage,
};

fn test_mutex_static() {
    static S: MutexStorage = MutexStorage::new();
    let m = Mutex::create(&S, 41u32).unwrap();
    {
        let mut g = m.lock().unwrap();
        *g += 1;
        assert_eq!(*g, 42);
    }
    assert_eq!(*m.lock().unwrap(), 42);
}

fn test_recursive_mutex_static() {
    static S: RecursiveMutexStorage = RecursiveMutexStorage::new();
    let m = RecursiveMutex::create(&S).unwrap();
    // Same thread re-locks — that's the recursive contract.
    let g1 = m.lock().unwrap();
    let g2 = m.lock().unwrap();
    drop(g2);
    drop(g1);
}

fn test_semaphore_static() {
    static S: SemaphoreStorage = SemaphoreStorage::new();
    let sem = Semaphore::create(&S, 1, 4).unwrap();
    sem.acquire().unwrap();
    assert!(sem.try_acquire().is_err()); // count is now 0
    sem.release();
    sem.try_acquire().unwrap();
}

fn test_event_static() {
    static S: EventStorage = EventStorage::new();
    let e = Event::create(&S).unwrap();
    e.signal(); // non-blocking; proves the handle is live
}

fn test_condvar_static() {
    static S: CondVarStorage = CondVarStorage::new();
    let cv = CondVar::create(&S).unwrap();
    cv.signal(); // non-blocking with no waiter
}

fn test_eventgroup_static() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    eg.set_bits(0b101);
    assert_eq!(eg.get_bits() & 0b101, 0b101);
}

fn test_queue_static() {
    static S: QueueStorage<u32, 4> = QueueStorage::new();
    let q: Queue<u32, 4> = Queue::create(&S).unwrap();
    q.send(&7u32).unwrap();
    assert_eq!(q.recv().unwrap(), 7u32);
}

fn test_stream_static() {
    static S: StreamStorage<64> = StreamStorage::new();
    let s: Stream<64> = Stream::create(&S, 1).unwrap();
    assert_eq!(s.send(&[1, 2, 3, 4]).unwrap(), 4);
    let mut buf = [0u8; 8];
    assert_eq!(s.recv(&mut buf).unwrap(), 4);
    assert_eq!(&buf[..4], &[1, 2, 3, 4]);
}

fn timer_noop() {}

fn test_timer_static() {
    static S: TimerStorage = TimerStorage::new();
    let t = Timer::create(&S, timer_noop, 1000, true).unwrap();
    t.start().unwrap();
    t.stop().unwrap();
}

pub fn run() -> (usize, usize) {
    run_suite(
        "ZeroHeapStatic",
        &[
            test_entry!(test_mutex_static),
            test_entry!(test_recursive_mutex_static),
            test_entry!(test_semaphore_static),
            test_entry!(test_event_static),
            test_entry!(test_condvar_static),
            test_entry!(test_eventgroup_static),
            test_entry!(test_queue_static),
            test_entry!(test_stream_static),
            test_entry!(test_timer_static),
        ],
    )
}
