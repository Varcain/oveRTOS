// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::{Error, Event, EventStorage, Semaphore, SemaphoreStorage};
#[cfg(zero_heap)]
use std::cell::UnsafeCell;
#[cfg(zero_heap)]
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicU32, Ordering};

#[cfg(sync_optional)]
use ove::ffi as bindings;
#[cfg(sync_optional)]
#[path = "../../bindings/rust/ove/src/async_runtime/critical_section.rs"]
mod actual_critical_section;

unsafe extern "C" {
    fn sync_test_interrupt(value: bool);
    fn sync_test_context(flags: u32);
    fn sync_test_count(kind: u32, index: u32) -> u32;
    fn sync_test_live(kind: u32) -> u32;
    fn sync_test_failure(kind: u32, value: u32);
    fn sync_test_gate(kind: u32, stage: u32, value: bool);
    fn sync_test_entered(kind: u32, stage: u32) -> bool;
    fn sync_test_registry_ok() -> bool;
    fn sync_test_cleanup_retained() -> u32;
    fn sync_test_clock_calls() -> u32;
    fn sync_test_forbidden_clock_calls() -> u32;
    #[cfg(sync_optional)]
    fn sync_test_actual_critical_depth() -> u64;
}

static CHECKS: AtomicU32 = AtomicU32::new(0);
static FAILURES: AtomicU32 = AtomicU32::new(0);
static WORKERS_STARTED: AtomicU32 = AtomicU32::new(0);
static WORKERS_DONE: AtomicU32 = AtomicU32::new(0);
const INIT: u32 = 0;
const CREATE: u32 = 1;
const DEINIT: u32 = 2;
const DESTROY: u32 = 3;
const FORBIDDEN: u32 = 5;
const NATIVE_INIT: u32 = 6;
const NATIVE_DEINIT: u32 = 7;
const WAIT: u32 = 8;
const TASK_POST: u32 = 9;
const ISR_POST: u32 = 10;
#[cfg(sync_optional)]
const NOTIFY: u32 = 11;
const HARNESS_CLEANUP: u32 = 12;
const COUNTERS: u32 = 13;

#[cfg(zero_heap)]
struct RawStorage<T>(UnsafeCell<MaybeUninit<T>>);
#[cfg(zero_heap)]
impl<T> RawStorage<T> {
    const fn new() -> Self {
        Self(UnsafeCell::new(MaybeUninit::zeroed()))
    }
    fn pointer(&self) -> *mut T {
        self.0.get().cast()
    }
}
// SAFETY: each closed raw case uses its independent extent serially; it stays
// live through final quiescent retirement even when cleanup is deliberately absent.
#[cfg(zero_heap)]
unsafe impl<T> Sync for RawStorage<T> {}

struct Context;
impl Context {
    fn new(flags: u32) -> Self {
        // SAFETY: selects bounded thread-local fixture metadata only.
        unsafe { sync_test_context(flags) };
        Self
    }
}
impl Drop for Context {
    fn drop(&mut self) {
        interrupt(false);
        // SAFETY: restore actual native context observation on this thread.
        unsafe { sync_test_context(u32::MAX) };
    }
}

struct Worker<T> {
    handle: Option<std::thread::JoinHandle<T>>,
    latch: Option<(u32, u32)>,
}
impl<T: Send + 'static> Worker<T> {
    fn spawn(f: impl FnOnce() -> T + Send + 'static, latch: Option<(u32, u32)>) -> Self {
        struct Completion;
        impl Drop for Completion {
            fn drop(&mut self) {
                WORKERS_DONE.fetch_add(1, Ordering::Release);
            }
        }
        let handle = std::thread::Builder::new()
            .stack_size(64 * 1024 * 1024)
            .spawn(move || {
                WORKERS_STARTED.fetch_add(1, Ordering::Release);
                let _completion = Completion;
                f()
            })
            .expect("host worker setup");
        Self {
            handle: Some(handle),
            latch,
        }
    }
    fn join(mut self) -> Option<T> {
        self.release_latch();
        match self.handle.take().unwrap().join() {
            Ok(value) => Some(value),
            Err(_) => {
                check(false, "worker_completed");
                None
            }
        }
    }
}
impl<T> Worker<T> {
    fn release_latch(&mut self) {
        if let Some((kind, stage)) = self.latch.take() {
            // SAFETY: release fixture metadata before waiting for the worker.
            unsafe { sync_test_gate(kind, stage, false) };
        }
    }
}
impl<T> Drop for Worker<T> {
    fn drop(&mut self) {
        self.release_latch();
        if let Some(handle) = self.handle.take() {
            if handle.join().is_err() {
                check(false, "worker_completed");
            }
        }
    }
}

fn check(value: bool, name: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !value {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        println!("FAIL: {name}");
    }
}

fn count<P: Primitive>(index: u32) -> u32 {
    // SAFETY: Reads synchronized bounded metadata without using a native handle.
    unsafe { sync_test_count(P::KIND, index) }
}

fn live<P: Primitive>() -> u32 {
    // SAFETY: Reads the synchronized registry without dereferencing its backing.
    unsafe { sync_test_live(P::KIND) }
}

fn interrupt(value: bool) {
    // SAFETY: Changes only this host thread's simulated context metadata.
    unsafe { sync_test_interrupt(value) }
}

fn failure<P: Primitive>(value: u32) {
    // SAFETY: Selects a bounded ordinary init failure before effects, or after
    // actual valid initialization and complete cleanup. No bad output is used.
    unsafe { sync_test_failure(P::KIND, value) }
}

fn gate<P: Primitive>(stage: u32, value: bool) {
    // SAFETY: Changes a synchronized bounded latch, never native object state.
    unsafe { sync_test_gate(P::KIND, stage, value) }
}

struct HeldGate {
    kind: u32,
    stage: u32,
}
impl HeldGate {
    fn new<P: Primitive>(stage: u32) -> Self {
        gate::<P>(stage, true);
        Self {
            kind: P::KIND,
            stage,
        }
    }
}
impl Drop for HeldGate {
    fn drop(&mut self) {
        // SAFETY: close the metadata latch even during parent unwinding.
        unsafe { sync_test_gate(self.kind, self.stage, false) };
    }
}

fn await_gate<P: Primitive>(stage: u32) {
    // A missing checkpoint is a controller failure, never a successful witness.
    // The outer owned-process deadline bounds this wait and all spawned threads.
    loop {
        // SAFETY: Reads synchronized bounded latch metadata, without a handle.
        if unsafe { sync_test_entered(P::KIND, stage) } {
            break;
        }
        std::thread::yield_now();
    }
}

trait Primitive: 'static {
    type Storage: Sync + 'static;
    type Owner: Send + Sync + 'static;
    const KIND: u32;
    const NAME: &'static str;
    fn first() -> &'static Self::Storage;
    fn control() -> &'static Self::Storage;
    fn second_control() -> &'static Self::Storage;
    fn create(storage: &'static Self::Storage) -> ove::Result<Self::Owner>;
    fn post(owner: &Self::Owner);
    fn wait(owner: &Self::Owner) -> ove::Result<()>;
    fn blocking(owner: &Self::Owner) -> ove::Result<()>;
    fn relative(owner: &Self::Owner) -> ove::Result<()>;
    fn post_n(owner: &Self::Owner, n: u32);
    fn named_post(owner: &Self::Owner);
    #[cfg(sync_optional)]
    fn until(owner: &Self::Owner, deadline: ove::time::Instant) -> ove::Result<()>;
    fn macro_owner() -> Self::Owner;
    #[cfg(zero_heap)]
    fn raw_control();
    #[cfg(zero_heap)]
    fn raw_interrupt_control();
}

struct Sem;
impl Primitive for Sem {
    type Storage = SemaphoreStorage;
    type Owner = Semaphore;
    const KIND: u32 = 0;
    const NAME: &'static str = "semaphore";
    fn first() -> &'static Self::Storage {
        static STORAGE: SemaphoreStorage = SemaphoreStorage::new();
        &STORAGE
    }
    fn control() -> &'static Self::Storage {
        static STORAGE: SemaphoreStorage = SemaphoreStorage::new();
        &STORAGE
    }
    fn second_control() -> &'static Self::Storage {
        static STORAGE: SemaphoreStorage = SemaphoreStorage::new();
        &STORAGE
    }
    fn create(storage: &'static Self::Storage) -> ove::Result<Self::Owner> {
        Semaphore::create(storage, 0, 3)
    }
    fn post(owner: &Self::Owner) {
        owner.release();
    }
    fn wait(owner: &Self::Owner) -> ove::Result<()> {
        owner.try_acquire()
    }
    fn blocking(owner: &Self::Owner) -> ove::Result<()> {
        owner.acquire()
    }
    fn relative(owner: &Self::Owner) -> ove::Result<()> {
        owner.try_acquire_for(core::time::Duration::ZERO)
    }
    fn post_n(owner: &Self::Owner, n: u32) {
        owner.release_n(n);
    }
    fn named_post(owner: &Self::Owner) {
        owner.release();
    }
    #[cfg(sync_optional)]
    fn until(owner: &Self::Owner, deadline: ove::time::Instant) -> ove::Result<()> {
        owner.try_acquire_until(deadline)
    }
    fn macro_owner() -> Self::Owner {
        ove::semaphore!(0, 3)
    }
    #[cfg(zero_heap)]
    fn raw_control() {
        static RAW: RawStorage<ove::ffi::ove_sem_storage_t> = RawStorage::new();
        // SAFETY: This exact native extent is aligned, exclusive and stable
        // through completed cleanup. No typed storage or other owner aliases it.
        let owner = unsafe { Semaphore::from_static(RAW.pointer(), 0, 3) };
        if let Ok(owner) = owner {
            exercise::<Self>(&owner);
            drop(owner);
        } else {
            check(false, "raw_exclusive_create");
        }
    }
    #[cfg(zero_heap)]
    fn raw_interrupt_control() {
        static RAW: RawStorage<ove::ffi::ove_sem_storage_t> = RawStorage::new();
        // SAFETY: Exact exclusive writable backing stays live throughout this
        // rejected initialization. The observer stops parent context violations
        // before effects; no owner or native initialized record is fabricated.
        let result = unsafe { Semaphore::from_static(RAW.pointer(), 0, 3) };
        check(
            matches!(result, Err(Error::NotSupported)),
            "raw_interrupt_create_rejected",
        );
        drop(result);
    }
}

struct Evt;
impl Primitive for Evt {
    type Storage = EventStorage;
    type Owner = Event;
    const KIND: u32 = 1;
    const NAME: &'static str = "event";
    fn first() -> &'static Self::Storage {
        static STORAGE: EventStorage = EventStorage::new();
        &STORAGE
    }
    fn control() -> &'static Self::Storage {
        static STORAGE: EventStorage = EventStorage::new();
        &STORAGE
    }
    fn second_control() -> &'static Self::Storage {
        static STORAGE: EventStorage = EventStorage::new();
        &STORAGE
    }
    fn create(storage: &'static Self::Storage) -> ove::Result<Self::Owner> {
        Event::create(storage)
    }
    fn post(owner: &Self::Owner) {
        owner.signal();
    }
    fn wait(owner: &Self::Owner) -> ove::Result<()> {
        owner.try_wait()
    }
    fn blocking(owner: &Self::Owner) -> ove::Result<()> {
        owner.wait()
    }
    fn relative(owner: &Self::Owner) -> ove::Result<()> {
        owner.wait_for(core::time::Duration::ZERO)
    }
    fn post_n(owner: &Self::Owner, n: u32) {
        for _ in 0..n {
            owner.signal();
        }
    }
    fn named_post(owner: &Self::Owner) {
        owner.signal_from_isr();
    }
    #[cfg(sync_optional)]
    fn until(owner: &Self::Owner, deadline: ove::time::Instant) -> ove::Result<()> {
        owner.wait_until(deadline)
    }
    fn macro_owner() -> Self::Owner {
        ove::event!()
    }
    #[cfg(zero_heap)]
    fn raw_control() {
        static RAW: RawStorage<ove::ffi::ove_event_storage_t> = RawStorage::new();
        // SAFETY: Complete aligned native backing is exclusive and remains
        // stable until this ordinary owner finishes native cleanup below.
        let owner = unsafe { Event::from_static(RAW.pointer()) };
        if let Ok(owner) = owner {
            exercise::<Self>(&owner);
            drop(owner);
        } else {
            check(false, "raw_exclusive_create");
        }
    }
    #[cfg(zero_heap)]
    fn raw_interrupt_control() {
        static RAW: RawStorage<ove::ffi::ove_event_storage_t> = RawStorage::new();
        // SAFETY: Exact aligned exclusive backing remains stable through this
        // rejected call. Forbidden parent entry is intercepted before effects.
        let result = unsafe { Event::from_static(RAW.pointer()) };
        check(
            matches!(result, Err(Error::NotSupported)),
            "raw_interrupt_create_rejected",
        );
        drop(result);
    }
}

fn exercise<P: Primitive>(owner: &P::Owner) {
    check(
        matches!(P::wait(owner), Err(Error::Timeout)),
        "empty_nonblocking_control",
    );
    P::post(owner);
    check(P::wait(owner).is_ok(), "post_wait_control");
}

fn create_checked<P: Primitive>(storage: &'static P::Storage, name: &str) -> Option<P::Owner> {
    let result = P::create(storage);
    check(result.is_ok(), name);
    result.ok()
}

fn controls<P: Primitive>() {
    let initialized = count::<P>(NATIVE_INIT);
    let cleaned = count::<P>(NATIVE_DEINIT);
    let forbidden = count::<P>(FORBIDDEN);
    for storage in [P::control(), P::second_control()] {
        if let Some(owner) = create_checked::<P>(storage, "independent_control_create") {
            exercise::<P>(&owner);
            drop(owner);
        }
    }
    check(
        count::<P>(NATIVE_INIT) == initialized + 2,
        "control_native_initialization",
    );
    check(
        count::<P>(NATIVE_DEINIT) == cleaned + 2,
        "control_native_cleanup",
    );
    check(
        count::<P>(FORBIDDEN) == forbidden,
        "control_no_forbidden_effect",
    );
}

fn duplicate<P: Primitive>() {
    let before = count::<P>(INIT);
    let second = P::create(P::first());
    #[cfg(zero_heap)]
    {
        check(matches!(second, Err(Error::Busy)), "live_repeat_busy");
        check(count::<P>(INIT) == before, "live_repeat_no_native_entry");
    }
    #[cfg(not(zero_heap))]
    {
        check(second.is_ok(), "heap_storage_independent");
        check(
            count::<P>(INIT) == before + 1,
            "heap_distinct_native_initialization",
        );
    }
    drop(second);
}

fn context_case<P: Primitive>(case: &str) {
    let (operation, mode) = case
        .strip_prefix("context_")
        .unwrap()
        .rsplit_once('_')
        .unwrap();
    let flags = match mode {
        "prestart" => 5,
        "masked" => 4,
        "resource" => 1,
        "unknown" => 0,
        _ => unreachable!(),
    };
    if operation == "create" {
        let init = count::<P>(INIT);
        let heap = count::<P>(CREATE);
        let context = Context::new(flags);
        let result = P::create(P::first());
        if flags & 1 == 0 {
            check(
                matches!(result, Err(Error::NotSupported)),
                "context_create_rejected",
            );
            check(
                count::<P>(INIT) == init && count::<P>(CREATE) == heap,
                "context_create_no_native_entry",
            );
        } else {
            check(result.is_ok(), "prestart_resource_create");
        }
        drop(context);
        if let Ok(owner) = result {
            exercise::<P>(&owner);
            drop(owner);
        }
        if let Some(owner) = create_checked::<P>(P::first(), "context_create_restored") {
            exercise::<P>(&owner);
            drop(owner);
        }
        return;
    }
    let Some(owner) = create_checked::<P>(P::first(), "context_operation_owner") else {
        return;
    };
    let waits = count::<P>(WAIT);
    let posts = count::<P>(TASK_POST);
    let isr_posts = count::<P>(ISR_POST);
    #[cfg(sync_optional)]
    let deadline = (operation == "deadline")
        .then(|| ove::time::Instant::now() + core::time::Duration::from_nanos(1000));
    let clocks = unsafe { sync_test_clock_calls() };
    let context = Context::new(flags);
    match operation {
        "try" | "relative" | "forever" => {
            let result = match operation {
                "try" => P::wait(&owner),
                "relative" => P::relative(&owner),
                _ => P::blocking(&owner),
            };
            check(
                matches!(result, Err(Error::NotSupported)),
                "context_wait_rejected",
            );
            check(count::<P>(WAIT) == waits, "context_wait_no_native_entry");
        }
        #[cfg(sync_optional)]
        "deadline" => {
            let result = P::until(&owner, deadline.unwrap());
            check(
                matches!(result, Err(Error::NotSupported)),
                "context_deadline_rejected",
            );
            check(count::<P>(WAIT) == waits, "context_deadline_no_wait_entry");
            check(
                unsafe { sync_test_clock_calls() } == clocks,
                "context_deadline_no_clock_entry",
            );
        }
        "post" | "named" | "many" => {
            P::post_n(&owner, 0);
            check(
                count::<P>(TASK_POST) == posts,
                "context_zero_posts_no_entry",
            );
            match operation {
                "post" => P::post(&owner),
                "named" => P::named_post(&owner),
                _ => P::post_n(&owner, 2),
            }
            let number = if operation == "many" { 2 } else { 1 };
            check(
                count::<P>(TASK_POST) == posts + if flags & 4 != 0 { number } else { 0 },
                "context_task_producer_dispatch",
            );
            check(
                count::<P>(ISR_POST) == isr_posts,
                "context_task_uses_no_isr_entry",
            );
        }
        _ => unreachable!(),
    }
    drop(context);
    if matches!(operation, "post" | "named" | "many") {
        let permits = if flags & 4 == 0 {
            0
        } else if operation == "many" && P::KIND == Sem::KIND {
            2
        } else {
            1
        };
        for _ in 0..permits {
            check(P::wait(&owner).is_ok(), "context_producer_delivered");
        }
        check(
            matches!(P::wait(&owner), Err(Error::Timeout)),
            "context_producer_exact_count",
        );
    }
    check(
        unsafe { sync_test_clock_calls() } == clocks,
        "context_operation_preserves_clock",
    );
    exercise::<P>(&owner);
    drop(owner);
}

#[cfg(sync_optional)]
fn masked_event(case: &str) {
    use std::sync::Arc;
    use std::task::{Wake, Waker};
    struct EventWake {
        event: Arc<Event>,
        masked_calls: AtomicU32,
    }
    impl Wake for EventWake {
        fn wake(self: Arc<Self>) {
            self.wake_by_ref();
        }
        fn wake_by_ref(self: &Arc<Self>) {
            // SAFETY: only observe actual POSIX TLS nesting, never a native handle.
            if unsafe { sync_test_actual_critical_depth() } > 0 {
                self.masked_calls.fetch_add(1, Ordering::Relaxed);
            }
            self.event.signal();
        }
    }
    let Some(owner) = create_checked::<Evt>(Evt::first(), "masked_event_owner") else {
        return;
    };
    let event = Arc::new(owner);
    let before = unsafe { sync_test_actual_critical_depth() };
    if case == "actual_waker" {
        let state = Arc::new(EventWake {
            event: event.clone(),
            masked_calls: AtomicU32::new(0),
        });
        let waker = Waker::from(state.clone());
        let registered = embassy_sync::waitqueue::AtomicWaker::new();
        registered.register(&waker);
        registered.wake();
        check(
            state.masked_calls.load(Ordering::Relaxed) == 1,
            "actual_atomic_waker_masked_tail",
        );
        check(event.try_wait().is_ok(), "actual_atomic_waker_delivered");
        drop(registered);
        drop(waker);
        drop(state);
    } else {
        critical_section::with(|_| {
            check(
                unsafe { sync_test_actual_critical_depth() } == before + 1,
                "actual_mask_entered",
            );
            if case == "actual_masked_named" {
                event.signal_from_isr();
            } else {
                event.signal();
            }
        });
        check(event.try_wait().is_ok(), "actual_masked_event_delivered");
    }
    check(
        unsafe { sync_test_actual_critical_depth() } == before,
        "actual_mask_restored",
    );
    check(
        count::<Evt>(TASK_POST) == 1 && count::<Evt>(ISR_POST) == 0,
        "actual_masked_task_dispatch",
    );
    exercise::<Evt>(&event);
    drop(event);
}

#[cfg(sync_optional)]
fn semaphore_notifications() {
    static HITS: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn notified(data: *mut core::ffi::c_void) {
        // SAFETY: this case registers the static HITS and serializes all gives
        // and registration/removal; the callback uses only its atomic counter.
        let hits = unsafe { &*data.cast::<AtomicU32>() };
        hits.fetch_add(1, Ordering::Relaxed);
    }
    let Some(owner) = create_checked::<Sem>(Sem::first(), "notify_owner") else {
        return;
    };
    let data = (&HITS as *const AtomicU32).cast_mut().cast();
    // SAFETY: static callback/data; registration and gives are sequential here.
    check(
        unsafe { owner.set_notify(Some(notified), data) }.is_ok(),
        "notify_installed",
    );
    owner.release_n(0);
    check(
        HITS.load(Ordering::Relaxed) == 0 && count::<Sem>(TASK_POST) == 0,
        "notify_zero_posts_no_callback",
    );
    critical_section::with(|_| {
        // SAFETY: no concurrent registration or give; static callback/data.
        check(
            unsafe { owner.set_notify(Some(notified), data) }.is_ok(),
            "masked_notify_setup",
        );
        owner.release();
    });
    check(HITS.load(Ordering::Relaxed) == 1, "masked_notify_delivered");
    check(owner.try_acquire().is_ok(), "masked_notify_permit");
    let prestart = Context::new(5);
    // SAFETY: serialized static callback setup in modeled prestart context.
    check(
        unsafe { owner.set_notify(Some(notified), data) }.is_ok(),
        "prestart_notify_setup",
    );
    owner.release_n(2);
    drop(prestart);
    check(
        HITS.load(Ordering::Relaxed) == 3,
        "prestart_notify_delivered",
    );
    check(
        owner.try_acquire().is_ok() && owner.try_acquire().is_ok(),
        "prestart_notify_permits",
    );
    for isr in [false, true] {
        let calls = count::<Sem>(NOTIFY);
        let posts = count::<Sem>(TASK_POST);
        let context = Context::new(0);
        interrupt(isr);
        // SAFETY: callback removal is serialized; rejected before native effects.
        let result = unsafe { owner.set_notify(None, core::ptr::null_mut()) };
        check(
            matches!(result, Err(Error::NotSupported)),
            "unsupported_notify_rejected",
        );
        check(
            count::<Sem>(NOTIFY) == calls,
            "unsupported_notify_no_native_entry",
        );
        owner.release_n(2);
        check(
            count::<Sem>(TASK_POST) == posts && HITS.load(Ordering::Relaxed) == 3,
            "unsupported_notify_no_post_or_callback",
        );
        drop(context);
    }
    owner.release();
    check(
        HITS.load(Ordering::Relaxed) == 4 && owner.try_acquire().is_ok(),
        "notify_restored_control",
    );
    // SAFETY: all prior gives returned and no worker can invoke the callback.
    check(
        unsafe { owner.set_notify(None, core::ptr::null_mut()) }.is_ok(),
        "notify_removed",
    );
    owner.release();
    check(
        HITS.load(Ordering::Relaxed) == 4 && owner.try_acquire().is_ok(),
        "removed_notify_has_no_callback",
    );
    exercise::<Sem>(&owner);
    drop(owner);
}

fn run<P: Primitive>(case: &str) {
    if case.starts_with("context_") {
        context_case::<P>(case);
    } else {
        match case {
            #[cfg(sync_optional)]
            "notify" => semaphore_notifications(),
            "invalid_counts" => {
                let result = Semaphore::create(Sem::first(), 4, 3);
                check(
                    matches!(result, Err(Error::InvalidParam)),
                    "invalid_counts_rejected",
                );
                drop(result);
                if let Some(owner) = create_checked::<Sem>(Sem::first(), "invalid_counts_retry") {
                    exercise::<Sem>(&owner);
                    drop(owner);
                }
            }
            #[cfg(sync_optional)]
            "actual_masked" | "actual_masked_named" | "actual_waker" => masked_event(case),
            "live" => {
                let owner = P::create(P::first()).unwrap();
                duplicate::<P>();
                exercise::<P>(&owner);
                drop(owner);
            }
            "concurrent_init" => {
                let _gate = HeldGate::new::<P>(0);
                let worker = Worker::spawn(|| P::create(P::first()), Some((P::KIND, 0)));
                await_gate::<P>(0);
                duplicate::<P>();
                check(
                    unsafe { sync_test_entered(P::KIND, 0) },
                    "contender_completed_during_initialization",
                );
                if let Some(result) = worker.join() {
                    check(result.is_ok(), "initializing_owner_completed");
                    if let Ok(owner) = result {
                        exercise::<P>(&owner);
                        drop(owner);
                    }
                }
                if let Some(owner) = create_checked::<P>(P::first(), "reuse_after_initialization") {
                    exercise::<P>(&owner);
                    drop(owner);
                }
            }
            "concurrent_live" => {
                let owner = P::create(P::first()).unwrap();
                Worker::spawn(duplicate::<P>, None).join();
                exercise::<P>(&owner);
                drop(owner);
            }
            "teardown" => {
                let owner = P::create(P::first()).unwrap();
                // Keep a parent-owned transport reference until spawn succeeds.
                // On setup failure its owner drops only after the latch guard.
                let transport = std::sync::Arc::new(std::sync::Mutex::new(Some(owner)));
                let _gate = HeldGate::new::<P>(1);
                let moved = transport.clone();
                let worker = Worker::spawn(
                    move || {
                        let owner = moved.lock().unwrap().take();
                        drop(owner);
                    },
                    Some((P::KIND, 1)),
                );
                await_gate::<P>(1);
                duplicate::<P>();
                worker.join();
                if let Some(owner) = create_checked::<P>(P::first(), "reuse_after_teardown") {
                    exercise::<P>(&owner);
                    drop(owner);
                }
            }
            "init_error" | "cleaned_error" => {
                let deinit = count::<P>(DEINIT);
                let native_deinit = count::<P>(NATIVE_DEINIT);
                let forbidden = count::<P>(FORBIDDEN);
                failure::<P>(if case == "init_error" { 1 } else { 2 });
                check(
                    matches!(P::create(P::first()), Err(Error::NoMemory)),
                    "init_error_reported",
                );
                check(live::<P>() == 0, "init_error_no_live_resource");
                check(count::<P>(DESTROY) == 0, "error_output_not_owned");
                let synthetic = u32::from(case == "cleaned_error");
                check(
                    count::<P>(DEINIT) == deinit + synthetic
                        && count::<P>(NATIVE_DEINIT) == native_deinit + synthetic
                        && count::<P>(FORBIDDEN) == forbidden,
                    "error_output_no_extra_cleanup",
                );
                failure::<P>(0);
                if let Some(owner) = create_checked::<P>(P::first(), "retry_after_error_cleanup") {
                    exercise::<P>(&owner);
                    drop(owner);
                }
            }
            "forget" | "drop_isr" | "drop_masked" | "drop_unknown" => {
                let owner = P::create(P::first()).unwrap();
                let deinit = count::<P>(DEINIT);
                let destroy = count::<P>(DESTROY);
                if case == "forget" {
                    std::mem::forget(owner);
                } else {
                    let _context = Context::new(if case == "drop_masked" { 4 } else { 0 });
                    interrupt(case == "drop_isr");
                    drop(owner);
                }
                check(count::<P>(DEINIT) == deinit, "retained_no_deinit_entry");
                check(count::<P>(DESTROY) == destroy, "retained_no_destroy_entry");
                duplicate::<P>();
                check(live::<P>() == 1, "retained_resource_stays_live");
            }
            "construct_isr" => {
                interrupt(true);
                let result = P::create(P::first());
                interrupt(false);
                check(
                    matches!(result, Err(Error::NotSupported)),
                    "interrupt_create_rejected",
                );
                check(count::<P>(INIT) == 0, "interrupt_create_no_init_entry");
                check(count::<P>(CREATE) == 0, "interrupt_create_no_heap_entry");
                drop(result);
                if let Some(owner) = create_checked::<P>(P::first(), "restored_construct_control") {
                    exercise::<P>(&owner);
                    drop(owner);
                }
            }
            "wait_isr" | "relative_isr" | "forever_isr" => {
                let owner = P::create(P::first()).unwrap();
                interrupt(true);
                let result = match case {
                    "relative_isr" => P::relative(&owner),
                    "forever_isr" => P::blocking(&owner),
                    _ => P::wait(&owner),
                };
                interrupt(false);
                check(
                    matches!(result, Err(Error::NotSupported)),
                    "interrupt_wait_rejected",
                );
                check(count::<P>(WAIT) == 0, "interrupt_wait_no_native_entry");
                exercise::<P>(&owner);
                drop(owner);
            }
            #[cfg(sync_optional)]
            "deadline_isr" => {
                let owner = P::create(P::first()).unwrap();
                let deadline = ove::time::Instant::now() + core::time::Duration::from_millis(1);
                interrupt(true);
                let result = P::until(&owner, deadline);
                interrupt(false);
                check(
                    matches!(result, Err(Error::NotSupported)),
                    "interrupt_deadline_rejected",
                );
                check(count::<P>(WAIT) == 0, "interrupt_deadline_no_wait_entry");
                // SAFETY: Reads synchronized scalar clock metadata, without a handle.
                check(
                    unsafe { sync_test_forbidden_clock_calls() } == 0,
                    "interrupt_deadline_no_clock_entry",
                );
                exercise::<P>(&owner);
                drop(owner);
            }
            "post_isr" => {
                let owner = P::create(P::first()).unwrap();
                interrupt(true);
                P::post(&owner);
                interrupt(false);
                check(count::<P>(TASK_POST) == 0, "interrupt_post_no_task_entry");
                if P::KIND == Evt::KIND {
                    check(count::<P>(ISR_POST) == 1, "interrupt_event_uses_isr_entry");
                    check(P::wait(&owner).is_ok(), "interrupt_event_delivered");
                } else {
                    check(
                        matches!(P::wait(&owner), Err(Error::Timeout)),
                        "interrupt_semaphore_no_effect",
                    );
                }
                exercise::<P>(&owner);
                drop(owner);
            }
            "named_producer" | "named_producer_isr" => {
                let owner = P::create(P::first()).unwrap();
                interrupt(case == "named_producer_isr");
                P::named_post(&owner);
                interrupt(false);
                let task = case == "named_producer";
                check(
                    count::<P>(TASK_POST) == u32::from(task),
                    "named_task_dispatch",
                );
                check(
                    count::<P>(ISR_POST) == u32::from(!task && P::KIND == Evt::KIND),
                    "named_interrupt_dispatch",
                );
                if task || P::KIND == Evt::KIND {
                    check(P::wait(&owner).is_ok(), "named_producer_delivered");
                } else {
                    check(
                        matches!(P::wait(&owner), Err(Error::Timeout)),
                        "named_semaphore_interrupt_no_effect",
                    );
                }
                exercise::<P>(&owner);
                drop(owner);
            }
            "macro_reuse" => {
                for _ in 0..2 {
                    let owner = P::macro_owner();
                    exercise::<P>(&owner);
                    drop(owner);
                }
            }
            "post_n" | "post_n_isr" => {
                let owner = P::create(P::first()).unwrap();
                let before = count::<P>(TASK_POST);
                P::post_n(&owner, 0);
                check(
                    count::<P>(TASK_POST) == before,
                    "zero_posts_no_native_entry",
                );
                interrupt(case == "post_n_isr");
                P::post_n(&owner, 2);
                interrupt(false);
                if case == "post_n_isr" {
                    check(
                        count::<P>(TASK_POST) == before,
                        "interrupt_multiple_posts_no_task_entry",
                    );
                }
                if P::KIND == Evt::KIND && case == "post_n_isr" {
                    check(
                        count::<P>(ISR_POST) == 2,
                        "interrupt_multiple_events_use_isr_entry",
                    );
                }
                let permits = if P::KIND == Evt::KIND {
                    1
                } else {
                    u32::from(case == "post_n") * 2
                };
                for _ in 0..permits {
                    check(P::wait(&owner).is_ok(), "multiple_posts_delivered");
                }
                check(
                    matches!(P::wait(&owner), Err(Error::Timeout)),
                    "multiple_posts_exact_or_coalesced",
                );
                drop(owner);
            }
            "handoff" => {
                let owner = std::sync::Arc::new(P::create(P::first()).unwrap());
                let waiter = owner.clone();
                let worker = Worker::spawn(move || P::blocking(&waiter), None);
                while count::<P>(WAIT) == 0 {
                    std::thread::yield_now();
                }
                P::post(&owner);
                check(
                    worker.join().is_some_and(|result| result.is_ok()),
                    "native_cross_thread_handoff",
                );
                exercise::<P>(&owner);
                drop(owner);
            }
            #[cfg(zero_heap)]
            "raw_construct_isr" => {
                interrupt(true);
                P::raw_interrupt_control();
                interrupt(false);
                check(count::<P>(INIT) == 0, "raw_interrupt_create_no_init_entry");
                P::raw_control();
            }
            #[cfg(zero_heap)]
            "raw_origin" => {
                let owner = P::create(P::first()).unwrap();
                P::raw_control();
                duplicate::<P>();
                exercise::<P>(&owner);
                drop(owner);
            }
            "reuse" => {
                for _ in 0..2 {
                    if let Some(owner) = create_checked::<P>(P::first(), "same_storage_reopens") {
                        exercise::<P>(&owner);
                        drop(owner);
                    }
                }
            }
            "normal" | "canary" => {}
            _ => panic!("unknown contract case"),
        }
    }
    controls::<P>();
    let retained = u32::from(matches!(
        case,
        "forget" | "drop_isr" | "drop_masked" | "drop_unknown"
    ));
    let before_retirement = live::<P>();
    check(before_retirement == retained, "final_resource_accounting");
    check(count::<P>(FORBIDDEN) == 0, "no_forbidden_native_entry");
    // SAFETY: Inspects live metadata and integer historical addresses only.
    check(
        unsafe { sync_test_registry_ok() },
        "registry_generation_accounting",
    );
    let before: Vec<_> = (0..COUNTERS).map(count::<P>).collect();
    // SAFETY: all accessible owners have dropped, contexts/latches restored and
    // all workers joined; raw extents are static. Retention was observed above.
    let cleaned = unsafe { sync_test_cleanup_retained() };
    check(
        cleaned == before_retirement,
        "retirement_matches_observed_live_generations",
    );
    check(live::<P>() == 0, "retirement_leaves_no_live_resource");
    check(
        count::<P>(NATIVE_INIT) == count::<P>(NATIVE_DEINIT),
        "native_lifecycle_balanced",
    );
    check(
        count::<P>(HARNESS_CLEANUP) == cleaned,
        "independent_cleanup_accounting",
    );
    let started = WORKERS_STARTED.load(Ordering::Acquire);
    let done = WORKERS_DONE.load(Ordering::Acquire);
    check(started == done, "all_workers_completed");
    let checks = CHECKS.load(Ordering::Relaxed);
    let failures = FAILURES.load(Ordering::Relaxed);
    let mode = if cfg!(zero_heap) { "zero" } else { "heap" };
    let counters: Vec<_> = (0..COUNTERS).map(count::<P>).collect();
    println!(
        "sync {mode}/{} {case}: checks={checks} failures={failures} retained={retained} cleanup={cleaned} workers={started},{done}",
        P::NAME
    );
    println!("native before: {before:?}");
    println!("native after: {counters:?}");
    println!(
        "clock: calls={} forbidden={}",
        unsafe { sync_test_clock_calls() },
        unsafe { sync_test_forbidden_clock_calls() }
    );
}

fn case_allowed(family: &str, case: &str) -> bool {
    if matches!(
        case,
        "normal"
            | "canary"
            | "live"
            | "concurrent_init"
            | "concurrent_live"
            | "teardown"
            | "init_error"
            | "cleaned_error"
            | "forget"
            | "drop_isr"
            | "drop_masked"
            | "drop_unknown"
            | "construct_isr"
            | "wait_isr"
            | "relative_isr"
            | "forever_isr"
            | "post_isr"
            | "named_producer"
            | "named_producer_isr"
            | "macro_reuse"
            | "post_n"
            | "post_n_isr"
            | "handoff"
            | "reuse"
    ) {
        return true;
    }
    if let Some((operation, mode)) = case
        .strip_prefix("context_")
        .and_then(|value| value.rsplit_once('_'))
    {
        return matches!(mode, "prestart" | "masked" | "resource" | "unknown")
            && (matches!(
                operation,
                "create" | "try" | "relative" | "forever" | "post" | "named" | "many"
            ) || operation == "deadline" && cfg!(sync_optional));
    }
    if case == "invalid_counts" {
        return family == "semaphore";
    }
    if matches!(case, "raw_construct_isr" | "raw_origin") {
        return cfg!(zero_heap);
    }
    if case == "deadline_isr" {
        return cfg!(sync_optional);
    }
    if case == "notify" {
        return cfg!(sync_optional) && family == "semaphore";
    }
    cfg!(sync_optional)
        && family == "event"
        && matches!(
            case,
            "actual_masked" | "actual_masked_named" | "actual_waker"
        )
}

fn main() -> std::process::ExitCode {
    let mut args = std::env::args().skip(1);
    let Some(family) = args.next() else {
        return std::process::ExitCode::from(2);
    };
    let Some(case) = args.next() else {
        return std::process::ExitCode::from(2);
    };
    if args.next().is_some()
        || !matches!(family.as_str(), "semaphore" | "event")
        || !case_allowed(&family, &case)
    {
        return std::process::ExitCode::from(2);
    }
    if case == "canary" {
        check(false, "early_calibration_survives_controls");
    }
    match family.as_str() {
        "semaphore" => run::<Sem>(&case),
        "event" => run::<Evt>(&case),
        _ => unreachable!(),
    }
    std::process::ExitCode::from(u8::from(FAILURES.load(Ordering::Relaxed) != 0))
}
