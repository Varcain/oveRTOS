// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::{Error, EventGroup, EventGroupStorage, WaitFlags};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant};

unsafe extern "C" {
    fn eventgroup_test_context(flags: u32);
    fn eventgroup_test_interrupt(value: bool);
    fn eventgroup_test_count(index: u32) -> u32;
    fn eventgroup_test_live() -> u32;
    fn eventgroup_test_failure(value: u32);
    fn eventgroup_test_gate(stage: u32, value: bool);
    fn eventgroup_test_entered(stage: u32) -> bool;
    fn eventgroup_test_cleanup_retained() -> u32;
    fn eventgroup_test_wait_result(code: i32);
    fn eventgroup_test_timeout() -> u64;
    fn eventgroup_test_irq_result(code: i32);
}
const INIT: u32 = 0;
const CREATE: u32 = 1;
const DEINIT: u32 = 2;
const DESTROY: u32 = 3;
const DUPLICATE: u32 = 4;
const FORBIDDEN: u32 = 5;
const SET: u32 = 8;
const CLEAR: u32 = 9;
const GET: u32 = 10;
const WAIT: u32 = 11;
const ISR_SET: u32 = 12;
const ISR_CLEAR: u32 = 13;
#[cfg(eventgroup_optional)]
const NOTIFY: u32 = 14;
#[cfg(eventgroup_optional)]
const CLOCK: u32 = 15;
static CHECKS: AtomicU32 = AtomicU32::new(0);
static FAILURES: AtomicU32 = AtomicU32::new(0);
static WORKERS: AtomicU32 = AtomicU32::new(0);
static DONE: AtomicU32 = AtomicU32::new(0);
#[cfg(eventgroup_optional)]
static NOTIFICATIONS: AtomicU32 = AtomicU32::new(0);
fn check(ok: bool, name: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !ok {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        println!("FAIL: {name}");
    }
}
fn count(index: u32) -> u32 {
    unsafe { eventgroup_test_count(index) }
}
fn entries() -> u32 {
    count(INIT) + count(CREATE)
}
fn cleanup() -> u32 {
    count(DEINIT) + count(DESTROY)
}
fn operations() -> u32 {
    count(SET) + count(CLEAR) + count(GET) + count(WAIT) + count(ISR_SET) + count(ISR_CLEAR)
}
struct Context;
impl Context {
    fn set(flags: u32, irq: bool) -> Self {
        unsafe {
            eventgroup_test_context(flags);
            eventgroup_test_interrupt(irq);
        }
        Self
    }
}
impl Drop for Context {
    fn drop(&mut self) {
        unsafe {
            eventgroup_test_interrupt(false);
            eventgroup_test_context(u32::MAX);
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
        WORKERS.fetch_add(1, Ordering::Release);
        let _done = Completion;
        f()
    })
}
struct Gate(u32);
impl Gate {
    fn new(stage: u32) -> Self {
        unsafe {
            eventgroup_test_gate(stage, true);
        }
        Self(stage)
    }
    fn wait(&self) -> bool {
        let end = Instant::now() + Duration::from_secs(3);
        while Instant::now() < end {
            if unsafe { eventgroup_test_entered(self.0) } {
                return true;
            }
            std::thread::yield_now();
        }
        false
    }
}
impl Drop for Gate {
    fn drop(&mut self) {
        unsafe {
            eventgroup_test_gate(self.0, false);
        }
    }
}

fn live_and_reuse() {
    static S: EventGroupStorage = EventGroupStorage::new();
    for _ in 0..2 {
        let first = EventGroup::create(&S);
        check(first.is_ok(), "complete_legal_cleanup_reopens_claim");
        let Ok(first) = first else {
            return;
        };
        let before = entries();
        let duplicate = count(DUPLICATE);
        let second = EventGroup::create(&S);
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
    let eg = EventGroup::create(&S).unwrap();
    check(eg.set_bits(3) == 3, "reopened_storage_has_new_state");
    drop(eg);
}
fn initialization_failure() {
    static S: EventGroupStorage = EventGroupStorage::new();
    for fault in [1, 2, 3] {
        unsafe {
            eventgroup_test_failure(fault);
        }
        let value = EventGroup::create(&S);
        check(
            matches!(value,Err(e) if e==if fault==3{Error::Unknown(-1234)}else{Error::NoMemory}),
            "construction_preserves_clean_error",
        );
        drop(value);
        let value = EventGroup::create(&S);
        check(value.is_ok(), "clean_failure_reopens_claim");
        drop(value);
    }
}
fn competing_initialization() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let gate = Gate::new(0);
    let first = worker(|| EventGroup::create(&S));
    check(gate.wait(), "initialization_latch_entered");
    let before = entries();
    let duplicates = count(DUPLICATE);
    let second = EventGroup::create(&S);
    check(
        if cfg!(zero_heap) {
            matches!(second, Err(Error::Busy))
        } else {
            second.is_ok()
        },
        "initializing_claim_admission",
    );
    check(
        !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicates),
        "initializing_claim_rejects_before_native",
    );
    drop(second);
    drop(gate);
    drop(first.join().unwrap());
}
fn competing_teardown() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    let gate = Gate::new(1);
    let first = worker(move || drop(eg));
    check(gate.wait(), "teardown_latch_entered");
    let before = entries();
    let duplicates = count(DUPLICATE);
    let second = EventGroup::create(&S);
    check(
        if cfg!(zero_heap) {
            matches!(second, Err(Error::Busy))
        } else {
            second.is_ok()
        },
        "tearing_down_claim_admission",
    );
    check(
        !cfg!(zero_heap) || (entries() == before && count(DUPLICATE) == duplicates),
        "tearing_down_rejects_before_native",
    );
    drop(second);
    drop(gate);
    first.join().unwrap();
    let eg = EventGroup::create(&S);
    check(eg.is_ok(), "complete_teardown_reopens_claim");
    drop(eg);
}
fn construction_contexts() {
    static S: EventGroupStorage = EventGroupStorage::new();
    for (flags, irq) in [(0, false), (2, false), (4, false), (7, true)] {
        let before = entries();
        let c = Context::set(flags, irq);
        let result = EventGroup::create(&S);
        check(
            matches!(result, Err(Error::NotSupported)),
            "constructor_requires_resource",
        );
        check(entries() == before, "constructor_rejects_before_native");
        drop(result);
        drop(c);
    }
    let c = Context::set(1, false);
    let eg = EventGroup::create(&S);
    check(eg.is_ok(), "resource_only_construction");
    drop(eg);
    drop(c);
}
fn operation_contexts() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    eg.set_bits(3);
    for (flags, irq) in [(0, false), (1, false), (4, false), (7, true)] {
        let before = operations();
        let c = Context::set(flags, irq);
        check(
            eg.set_bits(4) == 0
                && eg.set_bits(0) == 0
                && eg.clear_bits(0) == 0
                && eg.get_bits() == 0,
            "unsupported_scalars_are_inert_zero",
        );
        check(
            eg.wait_bits(1, WaitFlags::NONE) == Err(Error::NotSupported),
            "unsupported_forever_rejected",
        );
        check(
            eg.try_wait_bits(1, WaitFlags::NONE) == Err(Error::NotSupported),
            "unsupported_zero_wait_rejected",
        );
        check(
            eg.wait_bits_for(1, WaitFlags::NONE, Duration::ZERO) == Err(Error::NotSupported),
            "unsupported_duration_zero_rejected",
        );
        check(
            EventGroup::valid_mask() == 0x00ff_ffff,
            "pointer_free_mask_is_context_independent",
        );
        check(operations() == before, "unsupported_operations_skip_native");
        drop(c);
    }
    {
        let c = Context::set(2, false);
        check(eg.get_bits() == 3, "blocking_only_operations_admitted");
        drop(c);
    }
    check(eg.get_bits() == 3, "rejected_operations_preserve_state");
    #[cfg(eventgroup_optional)]
    {
        // SAFETY: actual POSIX critical-section cookies are nested and restored in order.
        let first = unsafe { ove::ffi::ove_irq_lock() };
        let second = unsafe { ove::ffi::ove_irq_lock() };
        let before = operations();
        check(
            eg.set_bits(8) == 0 && eg.try_wait_bits(1, WaitFlags::NONE) == Err(Error::NotSupported),
            "actual_nested_mask_rejects_task_access",
        );
        check(operations() == before, "actual_mask_skips_native"); // SAFETY: matching cookies for both still-held modeled critical sections.
        unsafe {
            ove::ffi::ove_irq_unlock(second);
            ove::ffi::ove_irq_unlock(first);
        }
    }
    drop(eg);
}
fn routing() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    eg.set_bits(8);
    let set = count(SET);
    let clear = count(CLEAR);
    let isrset = count(ISR_SET);
    let isrclear = count(ISR_CLEAR);
    check(
        eg.set_bits_from_isr(1) == 9,
        "task_isr_set_returns_full_task_snapshot",
    );
    check(
        count(SET) == set + 1 && count(ISR_SET) == isrset,
        "task_isr_set_uses_task_entry",
    );
    check(
        eg.clear_bits_from_isr(1) == Ok(()),
        "task_isr_clear_succeeds",
    );
    check(
        count(CLEAR) == clear + 1 && count(ISR_CLEAR) == isrclear,
        "task_isr_clear_uses_task_entry",
    );
    let before = operations();
    check(
        eg.clear_bits_from_isr(0x8000_0000) == Err(Error::InvalidParam),
        "task_isr_clear_preserves_invalid_param",
    );
    check(
        operations() == before,
        "invalid_task_isr_clear_skips_primitive",
    );
    check(
        eg.clear_bits_from_isr(0) == Ok(()),
        "task_isr_zero_clear_valid",
    );
    {
        let c = Context::set(4, false);
        let before = operations();
        check(
            eg.set_bits_from_isr(1) == 0 && eg.clear_bits_from_isr(1) == Err(Error::NotSupported),
            "unsupported_isr_named_task_calls_reject",
        );
        check(
            operations() == before,
            "unsupported_isr_named_task_skips_native",
        );
        drop(c);
    }
    {
        let c = Context::set(0, true);
        let set = count(SET);
        let clear = count(CLEAR);
        let a = count(ISR_SET);
        let b = count(ISR_CLEAR);
        check(
            eg.set_bits_from_isr(1) == 0x8000_0001,
            "interrupt_set_preserves_native_result",
        );
        check(
            eg.clear_bits_from_isr(1) == Ok(()),
            "interrupt_clear_admitted",
        );
        check(
            count(SET) == set
                && count(CLEAR) == clear
                && count(ISR_SET) == a + 1
                && count(ISR_CLEAR) == b + 1,
            "interrupt_uses_only_isr_entries",
        );
        for code in [-6, -1234] {
            unsafe {
                eventgroup_test_irq_result(code);
            }
            check(
                eg.clear_bits_from_isr(1)
                    == Err(if code == -6 {
                        Error::QueueFull
                    } else {
                        Error::Unknown(code)
                    }),
                "interrupt_clear_preserves_native_error",
            );
        }
        unsafe {
            eventgroup_test_irq_result(0);
        }
        check(
            eg.clear_bits_from_isr(0x8000_0000) == Err(Error::InvalidParam),
            "interrupt_clear_preserves_invalid_param",
        );
        drop(c);
    }
    drop(eg);
}
fn waits_and_durations() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    for (d, n) in [
        (Duration::ZERO, 0),
        (Duration::from_nanos(17), 17),
        (Duration::from_nanos(u64::MAX - 1), u64::MAX - 1),
        (Duration::from_nanos(u64::MAX), u64::MAX),
        (Duration::MAX, u64::MAX),
        (Duration::from_secs(u64::MAX), u64::MAX),
    ] {
        unsafe {
            eventgroup_test_wait_result(0);
        }
        check(
            eg.wait_bits_for(1, WaitFlags::NONE, d) == Ok(0xa5),
            "relative_wait_preserves_native_snapshot",
        );
        check(
            unsafe { eventgroup_test_timeout() } == n,
            "duration_conversion_saturates",
        );
    }
    for (code, error) in [(-4, Error::Timeout), (-1234, Error::Unknown(-1234))] {
        unsafe {
            eventgroup_test_wait_result(code);
        }
        check(
            eg.try_wait_bits(1, WaitFlags::NONE) == Err(error),
            "wait_preserves_native_error",
        );
    }
    check(
        eg.try_wait_bits(0, WaitFlags::NONE) == Err(Error::InvalidParam),
        "zero_wait_mask_rejected",
    );
    #[cfg(eventgroup_optional)]
    {
        let deadline = ove::time::Instant::now() + Duration::from_micros(1);
        let c = Context::set(4, false);
        let clocks = count(CLOCK);
        let waits = count(WAIT);
        check(
            eg.wait_bits_until(1, WaitFlags::NONE, deadline) == Err(Error::NotSupported),
            "deadline_context_before_clock",
        );
        check(
            count(CLOCK) == clocks && count(WAIT) == waits,
            "unsupported_deadline_skips_clock_and_native",
        );
        drop(c);
        let clocks = count(CLOCK);
        let waits = count(WAIT);
        check(
            eg.wait_bits_until(0, WaitFlags::NONE, deadline) == Err(Error::InvalidParam),
            "invalid_deadline_mask_rejected",
        );
        check(
            count(CLOCK) == clocks && count(WAIT) == waits,
            "invalid_deadline_mask_before_clock",
        );
        unsafe {
            eventgroup_test_wait_result(0);
        }
        check(
            eg.wait_bits_until(1, WaitFlags::NONE, deadline) == Ok(0xa5)
                && unsafe { eventgroup_test_timeout() } == 1000,
            "finite_deadline_conversion",
        );
        unsafe {
            eventgroup_test_wait_result(0);
        }
        check(
            eg.wait_bits_until(1, WaitFlags::NONE, ove::time::Instant::FOREVER) == Ok(0xa5)
                && unsafe { eventgroup_test_timeout() } == u64::MAX,
            "forever_deadline_conversion",
        );
    }
    drop(eg);
}
fn retained_owners() {
    static S: EventGroupStorage = EventGroupStorage::new();
    static F: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    let before = cleanup();
    let c = Context::set(4, false);
    drop(eg);
    check(cleanup() == before, "unsupported_drop_skips_native_cleanup");
    drop(c);
    let before = entries();
    let again = EventGroup::create(&S);
    check(
        !cfg!(zero_heap) || matches!(again, Err(Error::Busy)),
        "retained_owner_keeps_claim",
    );
    check(
        !cfg!(zero_heap) || entries() == before,
        "retained_claim_rejects_before_native",
    );
    drop(again);
    let eg = EventGroup::create(&F).unwrap();
    std::mem::forget(eg);
    let before = entries();
    let again = EventGroup::create(&F);
    check(
        !cfg!(zero_heap) || matches!(again, Err(Error::Busy)),
        "forgotten_owner_keeps_claim",
    );
    check(
        !cfg!(zero_heap) || entries() == before,
        "forgotten_claim_rejects_before_native",
    );
    drop(again);
}
#[cfg(zero_heap)]
fn raw_ownership() {
    static TYPED: EventGroupStorage = EventGroupStorage::new();
    static RAW: std::sync::Mutex<core::mem::MaybeUninit<ove::ffi::ove_eventgroup_storage_t>> =
        std::sync::Mutex::new(core::mem::MaybeUninit::uninit());
    let eg = EventGroup::create(&TYPED).unwrap();
    let mut storage = RAW.lock().unwrap();
    for (flags, irq) in [(0, false), (4, false), (7, true)] {
        let c = Context::set(flags, irq);
        let before = entries();
        // SAFETY: valid exclusive rooted backing; TLS admission is an inert observer.
        let denied = unsafe { EventGroup::from_static(storage.as_mut_ptr()) };
        check(
            matches!(denied, Err(Error::NotSupported)),
            "raw_constructor_requires_resource",
        );
        check(entries() == before, "raw_constructor_rejects_before_native");
        drop(denied);
        drop(c);
    }
    // SAFETY: one complete aligned rooted extent is exclusively held through legal cleanup.
    let raw = unsafe { EventGroup::from_static(storage.as_mut_ptr()) };
    check(raw.is_ok(), "raw_constructor_owns_distinct_backing");
    drop(raw);
    let before = entries();
    let other = EventGroup::create(&TYPED);
    check(
        matches!(other, Err(Error::Busy)) && entries() == before,
        "raw_cleanup_does_not_release_typed_claim",
    );
    drop(other);
    drop(eg);
}
fn native_integration() {
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = std::sync::Arc::new(EventGroup::create(&S).unwrap());
    check(eg.set_bits(3) == 3 && eg.get_bits() == 3, "native_set_get");
    check(
        eg.clear_bits(1) == 3 && eg.get_bits() == 2,
        "native_clear_full_prior",
    );
    check(
        eg.try_wait_bits(2, WaitFlags::CLEAR_ON_EXIT) == Ok(2) && eg.get_bits() == 0,
        "native_clear_on_exit",
    );
    check(
        eg.wait_bits_for(1, WaitFlags::NONE, Duration::from_millis(1)) == Err(Error::Timeout),
        "native_finite_timeout",
    );
    let sender = std::sync::Arc::clone(&eg);
    let th = worker(move || {
        sender.set_bits(4);
    });
    check(
        eg.wait_bits_for(4, WaitFlags::NONE, Duration::from_secs(1)) == Ok(4),
        "joined_worker_signals",
    );
    th.join().unwrap();
    drop(eg);
}
#[cfg(eventgroup_optional)]
fn notifications() {
    unsafe extern "C" fn notify(_: *mut core::ffi::c_void) {
        NOTIFICATIONS.fetch_add(1, Ordering::Relaxed);
    }
    static S: EventGroupStorage = EventGroupStorage::new();
    let eg = EventGroup::create(&S).unwrap();
    let before = count(NOTIFY);
    let c = Context::set(4, false);
    // SAFETY: no producers or old delivery tails exist; static code and null unused data.
    check(
        unsafe { eg.set_notify(Some(notify), core::ptr::null_mut()) } == Err(Error::NotSupported),
        "notify_requires_resource",
    );
    check(count(NOTIFY) == before, "notify_rejects_before_native");
    drop(c);
    // SAFETY: registration is serialized before producers; callback only increments static data.
    check(
        unsafe { eg.set_notify(Some(notify), core::ptr::null_mut()) } == Ok(()),
        "notify_setup_supported",
    );
    let before = NOTIFICATIONS.load(Ordering::Relaxed);
    eg.set_bits(1);
    check(
        NOTIFICATIONS.load(Ordering::Relaxed) == before + 1,
        "native_notify_after_set",
    );
    // SAFETY: all producers and synchronous tails are complete; removal is serialized.
    check(
        unsafe { eg.set_notify(None, core::ptr::null_mut()) } == Ok(()),
        "notify_removal_serialized",
    );
    drop(eg);
}
fn macro_reuse() {
    fn make() -> EventGroup {
        ove::eventgroup!()
    }
    let first = make();
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let before = entries();
    let second = std::panic::catch_unwind(make);
    check(
        if cfg!(zero_heap) {
            second.is_err()
        } else {
            second.is_ok()
        },
        "macro_live_admission",
    );
    check(
        !cfg!(zero_heap) || entries() == before,
        "macro_live_rejects_before_native",
    );
    drop(second);
    drop(first);
    let next = std::panic::catch_unwind(make);
    std::panic::set_hook(hook);
    check(
        matches!(&next, Ok(eg) if eg.get_bits() == 0),
        "macro_reuses_after_cleanup",
    );
    drop(next);
}
fn main() {
    if std::env::args().any(|x| x == "canary") {
        check(false, "early_failure_survives_controls");
    }
    for (name, f) in [
        ("live_and_reuse", live_and_reuse as fn()),
        ("initialization_failure", initialization_failure),
        ("competing_initialization", competing_initialization),
        ("competing_teardown", competing_teardown),
        ("construction_contexts", construction_contexts),
        ("operation_contexts", operation_contexts),
        ("routing", routing),
        ("waits_and_durations", waits_and_durations),
        ("retained_owners", retained_owners),
        ("native_integration", native_integration),
    ] {
        println!("case: {name}");
        f();
    }
    #[cfg(zero_heap)]
    {
        println!("case: raw_ownership");
        raw_ownership();
    }
    #[cfg(eventgroup_optional)]
    {
        println!("case: notifications");
        notifications();
    }
    println!("case: macro_reuse");
    macro_reuse();
    check(
        WORKERS.load(Ordering::Acquire) == DONE.load(Ordering::Acquire),
        "all_workers_joined",
    );
    check(count(FORBIDDEN) == 0, "all_native_entries_admitted");
    let live = unsafe { eventgroup_test_live() };
    let retired = unsafe { eventgroup_test_cleanup_retained() };
    check(
        retired == live && unsafe { eventgroup_test_live() } == 0,
        "retained_native_resources_retired_after_observation",
    );
    println!(
        "eventgroup contracts: checks={} failures={} workers={}/{} retired={} forbidden={}",
        CHECKS.load(Ordering::Relaxed),
        FAILURES.load(Ordering::Relaxed),
        WORKERS.load(Ordering::Relaxed),
        DONE.load(Ordering::Relaxed),
        retired,
        count(FORBIDDEN)
    );
    std::process::exit(i32::from(FAILURES.load(Ordering::Relaxed) != 0));
}
