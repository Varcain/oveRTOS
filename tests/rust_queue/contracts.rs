// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::{Error, Queue, QueueStorage};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant};

unsafe extern "C" {
    fn queue_test_context(flags: u32);
    fn queue_test_interrupt(value: bool);
    fn queue_test_count(index: u32) -> u32;
    fn queue_test_live() -> u32;
    fn queue_test_failure(value: u32);
    fn queue_test_gate(stage: u32, value: bool);
    fn queue_test_entered(stage: u32) -> bool;
    fn queue_test_cleanup_retained() -> u32;
}
const INIT: u32 = 0;
const CREATE: u32 = 1;
const DEINIT: u32 = 2;
const DESTROY: u32 = 3;
const DUPLICATE: u32 = 4;
const FORBIDDEN: u32 = 5;
const NATIVE_INIT: u32 = 6;
const NATIVE_DEINIT: u32 = 7;
const SEND: u32 = 8;
const RECEIVE: u32 = 9;
const ISR_SEND: u32 = 10;
const ISR_RECEIVE: u32 = 11;
const COUNT: u32 = 12;
const ISR_COUNT: u32 = 13;
const CAPACITY: u32 = 14;
#[cfg(queue_optional)]
const NOTIFY: u32 = 15;
const TASK_ZERO: u32 = 16;
const CLOCK: u32 = 17;
const FORBIDDEN_CLOCK: u32 = 18;
static CHECKS: AtomicU32 = AtomicU32::new(0);
static FAILURES: AtomicU32 = AtomicU32::new(0);
static STARTED: AtomicU32 = AtomicU32::new(0);
static DONE: AtomicU32 = AtomicU32::new(0);
#[cfg(queue_optional)]
static TAILS: AtomicU32 = AtomicU32::new(0);

fn check(ok: bool, name: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !ok {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        println!("FAIL: {name}");
    }
}
fn count(index: u32) -> u32 {
    // SAFETY: reads guarded fixture counters, never a native object.
    unsafe { queue_test_count(index) }
}
fn entries() -> u32 {
    count(INIT) + count(CREATE)
}
fn transfers() -> u32 {
    count(SEND) + count(RECEIVE) + count(ISR_SEND) + count(ISR_RECEIVE)
}
fn cleanup_entries() -> u32 {
    count(DEINIT) + count(DESTROY)
}
struct Context;
impl Context {
    fn set(flags: u32, irq: bool) -> Self {
        // SAFETY: supplies TLS observer metadata on an ordinary host thread.
        unsafe {
            queue_test_context(flags);
            queue_test_interrupt(irq);
        }
        Self
    }
}
impl Drop for Context {
    fn drop(&mut self) {
        // SAFETY: restores this ordinary thread's native observation.
        unsafe {
            queue_test_interrupt(false);
            queue_test_context(u32::MAX);
        }
    }
}
struct Completion;
impl Drop for Completion {
    fn drop(&mut self) {
        DONE.fetch_add(1, Ordering::Release);
    }
}
fn worker<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> std::thread::JoinHandle<T> {
    std::thread::spawn(move || {
        STARTED.fetch_add(1, Ordering::Release);
        let _completion = Completion;
        f()
    })
}
struct Gate(u32);
impl Gate {
    fn new(stage: u32) -> Self {
        // SAFETY: arms one fixture latch, consumed by its first visitor.
        unsafe { queue_test_gate(stage, true) };
        Self(stage)
    }
    fn wait(&self) -> bool {
        let end = Instant::now() + Duration::from_secs(3);
        while Instant::now() < end {
            // SAFETY: reads fixture latch metadata only.
            if unsafe { queue_test_entered(self.0) } {
                return true;
            }
            std::thread::yield_now();
        }
        false
    }
}
impl Drop for Gate {
    fn drop(&mut self) {
        // SAFETY: releases the visitor before the caller joins it.
        unsafe { queue_test_gate(self.0, false) };
    }
}

fn live_and_reuse() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    for _ in 0..2 {
        let Ok(first) = Queue::create(&S) else {
            check(false, "first_create");
            return;
        };
        check(true, "first_create");
        let before = entries();
        let duplicate = count(DUPLICATE);
        let second = Queue::create(&S);
        check(
            if cfg!(zero_heap) {
                matches!(second, Err(Error::Busy))
            } else {
                second.is_ok()
            },
            "live_storage_admission",
        );
        check(
            !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicate),
            "live_rejects_before_native",
        );
        drop(second);
        drop(first);
    }
    let Ok(q) = Queue::create(&S) else {
        check(false, "reuse_create");
        return;
    };
    check(
        q.try_send(&3).is_ok() && q.try_recv() == Ok(3),
        "reuse_native_payload",
    );
    drop(q);
    println!("case: live_and_reuse");
}
fn initialization_failure() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    for fault in [1, 2] {
        let before = count(NATIVE_INIT);
        let cleanup = count(NATIVE_DEINIT);
        // SAFETY: injects a fixture failure before publication, never a bad native object.
        unsafe { queue_test_failure(fault) };
        check(
            matches!(Queue::create(&S), Err(Error::NoMemory)),
            "clean_init_error",
        );
        check(
            count(NATIVE_INIT) - before == u32::from(fault == 2)
                && count(NATIVE_DEINIT) - cleanup == u32::from(fault == 2),
            "cleaned_error_balances_resources",
        );
        let Ok(q) = Queue::create(&S) else {
            check(false, "retry_after_error");
            continue;
        };
        check(
            q.try_send(&5).is_ok() && q.try_recv() == Ok(5),
            "retry_after_error",
        );
        drop(q);
    }
    println!("case: initialization_failure");
}
fn competing_initialization() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    let gate = Gate::new(0);
    let job = worker(|| Queue::create(&S));
    check(gate.wait(), "initialization_visitor_entered");
    let before = entries();
    let duplicate = count(DUPLICATE);
    let other = Queue::create(&S);
    check(
        if cfg!(zero_heap) {
            matches!(other, Err(Error::Busy))
        } else {
            other.is_ok()
        },
        "initializing_admission",
    );
    check(
        !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicate),
        "initializing_rejects_before_native",
    );
    if let Ok(q) = other {
        check(
            q.try_send(&1).is_ok() && q.try_recv() == Ok(1),
            "independent_heap_during_init",
        );
    }
    drop(gate);
    match job.join() {
        Ok(Ok(q)) => {
            check(
                q.try_send(&2).is_ok() && q.try_recv() == Ok(2),
                "initialization_worker_finishes",
            );
            drop(q);
        }
        _ => check(false, "initialization_worker_finishes"),
    }
    println!("case: competing_initialization");
}
fn competing_teardown() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    let Ok(q) = Queue::create(&S) else {
        check(false, "teardown_create");
        return;
    };
    let gate = Gate::new(1);
    let job = worker(move || drop(q));
    check(gate.wait(), "teardown_visitor_entered");
    let before = entries();
    let duplicate = count(DUPLICATE);
    let other = Queue::create(&S);
    check(
        if cfg!(zero_heap) {
            matches!(other, Err(Error::Busy))
        } else {
            other.is_ok()
        },
        "tearing_down_admission",
    );
    check(
        !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicate),
        "tearing_down_rejects_before_native",
    );
    drop(other);
    drop(gate);
    check(job.join().is_ok(), "teardown_worker_finishes");
    check(Queue::create(&S).is_ok(), "reuse_after_complete_teardown");
    println!("case: competing_teardown");
}

fn operation_contexts() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    let Ok(q) = Queue::create(&S) else {
        check(false, "context_create");
        return;
    };
    #[cfg(queue_optional)]
    let deadline = ove::time::Instant::now() + Duration::from_micros(1);
    for (flags, irq) in [(0, false), (1, false), (4, false), (128, false), (7, true)] {
        let _context = Context::set(flags, irq);
        let before = transfers();
        let clock = count(CLOCK);
        for result in [
            q.send(&1),
            q.try_send(&1),
            q.try_send_for(&1, Duration::ZERO),
            q.try_send_for(&1, Duration::from_nanos(1)),
        ] {
            check(result == Err(Error::NotSupported), "task_send_context");
        }
        for result in [
            q.recv(),
            q.try_recv(),
            q.try_recv_for(Duration::ZERO),
            q.try_recv_for(Duration::from_nanos(1)),
        ] {
            check(result == Err(Error::NotSupported), "task_receive_context");
        }
        #[cfg(queue_optional)]
        {
            check(
                q.try_send_until(&1, deadline) == Err(Error::NotSupported),
                "deadline_send_context",
            );
            check(
                q.try_recv_until(deadline) == Err(Error::NotSupported),
                "deadline_receive_context",
            );
        }
        check(
            transfers() == before && count(CLOCK) == clock,
            "task_rejection_before_effects",
        );
        let counts = count(COUNT) + count(ISR_COUNT);
        let cap = count(CAPACITY);
        let occupancy = q.len();
        check(
            occupancy == 0 && q.is_empty() && !q.is_full() && q.capacity() == 2,
            "inert_occupancy_policy",
        );
        check(
            count(COUNT) + count(ISR_COUNT) == counts && count(CAPACITY) == cap,
            "inert_occupancy_has_no_native_effects",
        );
        if !irq {
            let before = transfers();
            check(
                q.send_from_isr(&1) == Err(Error::NotSupported)
                    && q.receive_from_isr() == Err(Error::NotSupported),
                "unsupported_task_isr_names",
            );
            check(
                q.len_from_isr() == 0
                    && transfers() == before
                    && count(COUNT) + count(ISR_COUNT) == counts,
                "unsupported_task_isr_names_before_effects",
            );
        }
    }
    check(
        q.try_send(&9).is_ok() && q.try_recv() == Ok(9),
        "context_controls_restored",
    );
    println!("case: operation_contexts");
}
fn transfer_routing() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    let Ok(q) = Queue::create(&S) else {
        check(false, "routing_create");
        return;
    };
    let task = count(TASK_ZERO);
    let irq = count(ISR_SEND) + count(ISR_RECEIVE) + count(ISR_COUNT);
    check(
        q.send_from_isr(&7).is_ok() && q.len_from_isr() == 1 && q.receive_from_isr() == Ok(7),
        "task_isr_names_route_to_task",
    );
    check(
        count(TASK_ZERO) == task + 2
            && count(ISR_SEND) + count(ISR_RECEIVE) + count(ISR_COUNT) == irq,
        "task_isr_names_exact_entries",
    );
    let before = transfers();
    let irq_before = count(ISR_SEND) + count(ISR_RECEIVE) + count(ISR_COUNT);
    {
        let _context = Context::set(7, true);
        check(
            q.send_from_isr(&8).is_ok() && q.len_from_isr() == 1 && q.receive_from_isr() == Ok(8),
            "modeled_irq_transfer",
        );
    }
    check(
        transfers() == before + 2
            && count(ISR_SEND) + count(ISR_RECEIVE) + count(ISR_COUNT) == irq_before + 3,
        "modeled_irq_exact_entries",
    );
    check(
        q.capacity() == 2 && q.is_empty(),
        "routing_controls_restored",
    );
    println!("case: transfer_routing");
}

fn construction_contexts() {
    static S: QueueStorage<u32, 1> = QueueStorage::new();
    for (flags, irq) in [(0, false), (4, false), (128, false), (7, true)] {
        let _context = Context::set(flags, irq);
        let before = entries();
        check(
            matches!(Queue::create(&S), Err(Error::NotSupported)),
            "construction_context",
        );
        check(entries() == before, "construction_before_native");
    }
    let _context = Context::set(1, false);
    let cleanup = cleanup_entries();
    check(
        Queue::create(&S).is_ok(),
        "resource_only_construction_and_drop",
    );
    check(cleanup_entries() > cleanup, "resource_only_cleanup");
    println!("case: construction_contexts");
}
fn retained_owners() {
    static S: [QueueStorage<u32, 1>; 5] = [const { QueueStorage::new() }; 5];
    let policies = [(0, false), (4, false), (128, false), (7, true)];
    for (storage, (flags, irq)) in S.iter().zip(policies) {
        let Ok(q) = Queue::create(storage) else {
            check(false, "retained_create");
            continue;
        };
        let before = cleanup_entries();
        {
            let _context = Context::set(flags, irq);
            drop(q);
        }
        check(
            cleanup_entries() == before,
            "unsupported_drop_has_no_native_cleanup",
        );
        let before = entries();
        let duplicate = count(DUPLICATE);
        let retry = Queue::create(storage);
        check(
            if cfg!(zero_heap) {
                matches!(retry, Err(Error::Busy))
            } else {
                retry.is_ok()
            },
            "retained_claim_admission",
        );
        check(
            !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicate),
            "retained_claim_before_native",
        );
        drop(retry);
    }
    if let Ok(q) = Queue::create(&S[4]) {
        let before = cleanup_entries();
        std::mem::forget(q);
        check(cleanup_entries() == before, "forget_has_no_native_cleanup");
        let before = entries();
        let duplicate = count(DUPLICATE);
        let retry = Queue::create(&S[4]);
        check(
            if cfg!(zero_heap) {
                matches!(retry, Err(Error::Busy))
            } else {
                retry.is_ok()
            },
            "forgotten_claim_admission",
        );
        check(
            !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicate),
            "forgotten_claim_before_native",
        );
        drop(retry);
    } else {
        check(false, "forgotten_create");
    }
    println!("case: retained_owners");
}

fn native_integration() {
    static S: QueueStorage<u32, 2> = QueueStorage::new();
    let Ok(q) = Queue::create(&S) else {
        check(false, "integration_create");
        return;
    };
    check(q.try_recv() == Err(Error::QueueEmpty), "empty_queue");
    check(q.send(&1).is_ok() && q.try_send(&2).is_ok(), "fifo_fill");
    check(
        q.len() == 2 && q.capacity() == 2 && q.is_full() && !q.is_empty(),
        "fifo_occupancy",
    );
    check(q.try_send(&3) == Err(Error::QueueFull), "full_queue");
    check(
        q.try_send_for(&3, Duration::from_nanos(1)) == Err(Error::Timeout),
        "full_finite_timeout",
    );
    check(q.recv() == Ok(1) && q.try_recv() == Ok(2), "fifo_order");
    check(
        q.try_recv_for(Duration::from_nanos(1)) == Err(Error::Timeout),
        "empty_finite_timeout",
    );
    let sum = std::thread::scope(|scope| {
        let queue = &q;
        let job = scope.spawn(move || {
            STARTED.fetch_add(1, Ordering::Release);
            let _completion = Completion;
            let mut sum = 0;
            for _ in 0..5 {
                if let Ok(n) = queue.try_recv_for(Duration::from_millis(500)) {
                    sum += n;
                }
            }
            sum
        });
        for n in 1..=5 {
            check(
                q.try_send_for(&n, Duration::from_millis(500)).is_ok(),
                "producer_transfer",
            );
        }
        job.join().unwrap_or_default()
    });
    check(sum == 15 && q.is_empty(), "joined_consumer_payload");
    drop(q);
    check(Queue::create(&S).is_ok(), "integration_reuse");
    println!("case: native_integration");
}

fn dimension_rejection() {
    static ZERO_CAP: QueueStorage<u8, 0> = QueueStorage::new();
    static ZERO_ITEM: QueueStorage<(), 1> = QueueStorage::new();
    let before = entries();
    check(
        matches!(Queue::create(&ZERO_CAP), Err(Error::InvalidParam)),
        "zero_capacity",
    );
    check(
        matches!(Queue::create(&ZERO_ITEM), Err(Error::InvalidParam)),
        "zero_item",
    );
    check(entries() == before, "dimensions_before_native");
    #[cfg(not(zero_heap))]
    {
        let before = entries();
        check(
            matches!(
                Queue::<u8, { u32::MAX as usize + 1 }>::new(),
                Err(Error::InvalidParam)
            ),
            "capacity_conversion",
        );
        // Only the type's size is evaluated. No enormous payload is constructed.
        check(
            matches!(
                Queue::<[u8; 1usize << 33], { u32::MAX as usize }>::new(),
                Err(Error::InvalidParam)
            ),
            "extent_overflow",
        );
        check(entries() == before, "large_dimensions_before_native");
    }
    println!("case: dimension_rejection");
}

#[cfg(zero_heap)]
fn raw_ownership() {
    use core::cell::UnsafeCell;
    use core::mem::MaybeUninit;
    struct Raw {
        storage: UnsafeCell<MaybeUninit<ove::ffi::ove_queue_storage_t>>,
        buffer: UnsafeCell<MaybeUninit<[u32; 2]>>,
    }
    // SAFETY: the closed case accesses each extent serially through one raw owner;
    // these fields stay stable, aligned and disjoint through final retirement.
    unsafe impl Sync for Raw {}
    static RAW: Raw = Raw {
        storage: UnsafeCell::new(MaybeUninit::zeroed()),
        buffer: UnsafeCell::new(MaybeUninit::uninit()),
    };
    for _ in 0..2 {
        let before = count(DEINIT);
        // SAFETY: the previous generation finished cleanup; this static complete
        // backing has no other raw, wrapper or payload reference access.
        let q = unsafe {
            Queue::<u32, 2>::from_static(RAW.storage.get().cast(), RAW.buffer.get().cast())
        };
        if let Ok(q) = q {
            check(
                q.try_send(&19).is_ok() && q.try_recv() == Ok(19),
                "raw_payload",
            );
            drop(q);
            check(count(DEINIT) == before + 1, "raw_cleanup_exactly_once");
        } else {
            check(false, "raw_create");
        }
    }
    println!("case: raw_ownership");
}

#[cfg(not(zero_heap))]
fn borrowed_payloads() {
    let value = 37;
    let q = Queue::<&u32, 1>::new();
    if let Ok(q) = q {
        check(
            q.try_send(&&value).is_ok() && q.try_recv() == Ok(&value),
            "borrowed_heap_payload",
        );
        drop(q);
    } else {
        check(false, "borrowed_create");
    }
    {
        let local = 41;
        let q = Queue::<&u32, 1>::new();
        if let Ok(q) = q {
            check(q.try_send(&&local).is_ok(), "borrowed_retained_bytes");
            let before = cleanup_entries();
            let _context = Context::set(0, false);
            drop(q);
            check(
                cleanup_entries() == before,
                "borrowed_retention_before_cleanup",
            );
        } else {
            check(false, "borrowed_retention_create");
        }
    }
    // The inaccessible foreign bytes outlive local; retirement discards them.
    println!("case: borrowed_payloads");
}

fn local_storage_payload() {
    static S: QueueStorage<*mut u8, 1> = QueueStorage::new();
    let mut local = 23u8;
    if let Ok(q) = Queue::create(&S) {
        let pointer = &raw mut local;
        check(
            q.try_send(&pointer).is_ok() && q.try_recv() == Ok(pointer),
            "local_non_send_payload",
        );
        drop(q);
    } else {
        check(false, "local_non_send_create");
    }
    println!("case: local_storage_payload");
}

#[cfg(queue_optional)]
fn notifications() {
    unsafe extern "C" fn notify(_: *mut core::ffi::c_void) {
        TAILS.fetch_add(1, Ordering::Relaxed);
    }
    static S: QueueStorage<u32, 1> = QueueStorage::new();
    let Ok(q) = Queue::create(&S) else {
        check(false, "notify_create");
        return;
    };
    {
        let _context = Context::set(1, false);
        // SAFETY: no producer or registration overlaps setup. Callback code and
        // atomic state are static, support concurrent calls and never unwind.
        check(
            unsafe { q.set_notify(Some(notify), core::ptr::null_mut()) }.is_ok(),
            "resource_notify_setup",
        );
    }
    check(
        q.try_send(&1).is_ok() && q.try_recv() == Ok(1) && TAILS.load(Ordering::Relaxed) == 1,
        "notify_tail_completed",
    );
    for (flags, irq) in [(0, false), (4, false), (128, false), (7, true)] {
        let _context = Context::set(flags, irq);
        let before = count(NOTIFY);
        // SAFETY: same serialized static callback; metadata alone is unsupported.
        check(
            unsafe { q.set_notify(Some(notify), core::ptr::null_mut()) }
                == Err(Error::NotSupported),
            "notify_context",
        );
        check(count(NOTIFY) == before, "notify_before_native");
    }
    {
        // SAFETY: nest and restore actual POSIX critical-section cookies in order.
        let first = unsafe { ove::ffi::ove_irq_lock() };
        let second = unsafe { ove::ffi::ove_irq_lock() };
        let before = transfers();
        check(
            q.try_send(&2) == Err(Error::NotSupported),
            "actual_masked_task_rejected",
        );
        check(transfers() == before, "actual_masked_before_native");
        // SAFETY: matching cookies for the two still-held critical sections.
        unsafe {
            ove::ffi::ove_irq_unlock(second);
            ove::ffi::ove_irq_unlock(first);
        }
    }
    // SAFETY: all synchronous producer tails completed before serialized removal.
    check(
        unsafe { q.set_notify(None, core::ptr::null_mut()) }.is_ok(),
        "notify_removal",
    );
    drop(q);
    println!("case: notifications");
}

fn macro_reuse() {
    fn create() -> Queue<u32, 2> {
        ove::queue!(u32, 2)
    }
    for n in 1..=2 {
        let q = create();
        check(
            q.try_send(&n).is_ok() && q.try_recv() == Ok(n),
            "macro_native_reuse",
        );
        drop(q);
    }
    println!("case: macro_reuse");
}

fn main() {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if !args.is_empty() && args != ["canary"] {
        std::process::exit(2);
    }
    if !args.is_empty() {
        check(false, "early_failure_survives_controls");
    }
    live_and_reuse();
    initialization_failure();
    competing_initialization();
    competing_teardown();
    construction_contexts();
    operation_contexts();
    transfer_routing();
    retained_owners();
    native_integration();
    dimension_rejection();
    local_storage_payload();
    #[cfg(zero_heap)]
    raw_ownership();
    #[cfg(not(zero_heap))]
    borrowed_payloads();
    #[cfg(queue_optional)]
    notifications();
    macro_reuse();
    check(
        STARTED.load(Ordering::Acquire) == 3 && DONE.load(Ordering::Acquire) == 3,
        "workers_joined",
    );
    check(
        count(FORBIDDEN) == 0 && count(DUPLICATE) == 0 && count(FORBIDDEN_CLOCK) == 0,
        "no_forbidden_native_entries",
    );
    // SAFETY: every owner and scoped worker is gone; all gates and TLS contexts restored.
    let retained = unsafe { queue_test_cleanup_retained() };
    check(
        retained == if cfg!(zero_heap) { 5 } else { 6 },
        "retained_generations_measured",
    );
    // SAFETY: observes the now-quiescent registry only.
    check(
        unsafe { queue_test_live() } == 0 && count(NATIVE_INIT) == count(NATIVE_DEINIT),
        "all_native_resources_retired",
    );
    let failures = FAILURES.load(Ordering::Relaxed);
    println!(
        "queue contracts: checks={} failures={failures} workers=3/3 retained={retained}",
        CHECKS.load(Ordering::Relaxed)
    );
    std::process::exit(i32::from(failures != 0));
}
