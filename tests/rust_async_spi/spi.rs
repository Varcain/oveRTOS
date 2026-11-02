// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Wake, Waker};

use ove::spi::{ChipSelect, Config};
use ove::{AsyncSpiStorage, Error};

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
struct Stats {
    allocations: usize,
    frees: usize,
    live: usize,
    bytes: usize,
    peak_bytes: usize,
    attempts: usize,
    submissions: usize,
    completions: usize,
    opens: usize,
    closes: usize,
    asserted: usize,
    deasserted: usize,
    mutex_inits: usize,
    mutex_deinits: usize,
}

unsafe extern "C" {
    fn async_spi_probe_reset();
    fn async_spi_probe_stats(out: *mut Stats);
    fn async_spi_probe_fail_alloc(after: i32);
    fn async_spi_probe_prepare_error(error: i32);
    fn async_spi_probe_open_error(error: i32);
    fn async_spi_probe_mode(mode: i32);
    fn async_spi_probe_limit(words: usize);
    fn async_spi_probe_complete(result: i32);
    fn async_spi_probe_last_bytes() -> usize;
    fn async_spi_probe_last_words() -> usize;
    fn async_spi_probe_last_port() -> u32;
    fn async_spi_probe_tx_byte(index: usize) -> u32;
    fn async_spi_probe_tx_tail() -> u32;
    fn async_spi_probe_pending_tx() -> usize;
    fn async_spi_probe_pending_rx() -> usize;
    fn async_spi_probe_irq_depth() -> u32;
    fn async_spi_probe_local_irq_model(enabled: i32);
}

static SERIAL: Mutex<()> = Mutex::new(());
struct Fixture {
    _serial: MutexGuard<'static, ()>,
}
impl Fixture {
    fn new() -> Self {
        let serial = SERIAL.lock().unwrap_or_else(|error| error.into_inner());
        unsafe { async_spi_probe_reset() };
        Self { _serial: serial }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            let state = stats();
            assert_eq!(
                state.live, 0,
                "all binding/native heap allocations reclaimed: {state:?}"
            );
            assert_eq!(state.bytes, 0);
            assert_eq!(state.allocations, state.frees);
            assert_eq!(
                state.opens, state.closes,
                "all initialized native controllers closed: {state:?}"
            );
            assert_eq!(state.mutex_inits, state.mutex_deinits);
        }
    }
}
fn stats() -> Stats {
    let mut state = Stats::default();
    unsafe { async_spi_probe_stats(&mut state) };
    state
}
fn poll<F: Future>(future: &mut Pin<Box<F>>) -> Poll<F::Output> {
    future
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
}
fn poll_stack<F: Future + Unpin>(future: &mut F) -> Poll<F::Output> {
    Pin::new(future).poll(&mut Context::from_waker(Waker::noop()))
}
fn finish<F: Future<Output = ove::Result<()>>>(future: &mut Pin<Box<F>>, result: i32) {
    unsafe { async_spi_probe_complete(result) };
    assert_eq!(poll(future), Poll::Ready(Error::from_code(result)));
}

#[test]
fn unequal_spans_use_owned_zero_padded_single_operation() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<16> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    for (write, read) in [(2, 4), (4, 1), (4, 4), (0, 4), (4, 0)] {
        let tx = [1, 2, 3, 4];
        let mut rx = [0xcc; 6];
        let source = tx.as_ptr() as usize;
        let destination = rx.as_mut_ptr() as usize;
        let before = stats();
        let cs = ChipSelect::new(2, 7, true);
        let mut future = Box::pin(spi.transfer(Some(&cs), &tx[..write], &mut rx[..read]));
        assert!(poll(&mut future).is_pending());
        unsafe {
            assert_ne!(async_spi_probe_pending_tx(), source);
            assert_ne!(async_spi_probe_pending_rx(), destination);
            assert_eq!(async_spi_probe_last_bytes(), write.max(read));
            assert_eq!(async_spi_probe_last_words(), write.max(read));
        }
        finish(&mut future, 0);
        drop(future);
        for index in 0..read {
            assert_eq!(rx[index], if index < write { tx[index] } else { 0 });
        }
        assert_eq!(&rx[read..], &vec![0xcc; 6 - read]);
        for index in 0..write.max(read) {
            assert_eq!(
                unsafe { async_spi_probe_tx_byte(index) },
                if index < write { tx[index] as u32 } else { 0 }
            );
        }
        let after = stats();
        assert_eq!(after.submissions, before.submissions + 1);
        assert_eq!(after.asserted, before.asserted + 1);
        assert_eq!(after.deasserted, before.deasserted + 1);
    }
}

#[test]
fn dropped_and_forgotten_futures_retain_only_owned_spans_and_cs() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<16> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    for forget in [false, true] {
        let mut tx = [1, 2, 3, 4];
        let mut rx = [0; 4];
        let mut cs = ChipSelect::new(2, 7, true);
        let mut future = spi.transfer(Some(&cs), &tx, &mut rx);
        assert!(poll_stack(&mut future).is_pending());
        if forget {
            std::mem::forget(future);
        } else {
            drop(future);
        }
        tx.fill(0xe1);
        rx.fill(0xcc);
        cs = ChipSelect::new(9, 7, true);
        unsafe { async_spi_probe_complete(0) };
        assert_eq!(
            rx, [0xcc; 4],
            "native completion wrote released application RX"
        );
        assert_eq!(
            unsafe { async_spi_probe_tx_byte(0) },
            1,
            "native completion read reused application TX"
        );
        assert_eq!(
            unsafe { async_spi_probe_last_port() },
            2,
            "native completion read caller CS"
        );
        let mut flush = Box::pin(spi.flush());
        assert_eq!(poll(&mut flush), Poll::Ready(Ok(())));
        drop(flush);
        let mut next = Box::pin(spi.write(Some(&cs), &tx));
        assert!(poll(&mut next).is_pending());
        finish(&mut next, 0);
        drop(next);
        assert_eq!(unsafe { async_spi_probe_last_port() }, 9);
    }
}

#[test]
fn orphan_busy_empty_and_real_flush_preserve_operation() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    let mut first = spi.write(None, &[1, 2]);
    assert!(poll_stack(&mut first).is_pending());
    std::mem::forget(first);
    let pointer = unsafe { async_spi_probe_pending_tx() };
    let before = stats();
    let mut empty = [];
    let mut future = Box::pin(spi.transfer(None, &[], &mut empty));
    assert_eq!(poll(&mut future), Poll::Ready(Ok(())));
    drop(future);
    let mut busy = Box::pin(spi.write(None, &[3]));
    assert_eq!(poll(&mut busy), Poll::Ready(Err(Error::BusBusy)));
    drop(busy);
    assert_eq!(unsafe { async_spi_probe_pending_tx() }, pointer);
    assert_eq!(stats().submissions, before.submissions);
    let mut flush = Box::pin(spi.flush());
    assert!(poll(&mut flush).is_pending());
    finish(&mut flush, -29);
    drop(flush);
    let mut again = Box::pin(spi.flush());
    assert_eq!(poll(&mut again), Poll::Ready(Ok(())));
}

#[test]
fn dropped_orphan_rejects_new_operation_before_completion() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let mut first = owner.write(None, &[1, 2]);
    assert!(poll_stack(&mut first).is_pending());
    drop(first);
    let pending = unsafe { async_spi_probe_pending_tx() };
    let before = stats();
    let rejected = poll_stack(&mut owner.write(None, &[9, 8]));
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(rejected, Poll::Ready(Err(Error::BusBusy)));
    assert_eq!(stats().submissions, before.submissions);
    assert_eq!(
        unsafe { async_spi_probe_tx_byte(0) },
        1,
        "new operation overwrote pending TX"
    );
    assert_ne!(pending, 0);
    assert_eq!(poll_stack(&mut owner.flush()), Poll::Ready(Ok(())));
}

#[test]
fn pending_owner_move_drop_defers_teardown_and_static_reuse() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    let mut future = Box::pin(spi.write(None, &[1, 2]));
    assert!(poll(&mut future).is_pending());
    drop(future);
    let moved = Some(spi).take().unwrap();
    drop(moved);
    assert_eq!(
        stats().closes,
        0,
        "pending owner closed before native retirement"
    );
    assert!(matches!(STORAGE.init(Config::default()), Err(Error::Busy)));
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(stats().closes, 1);
    drop(STORAGE.init(Config::default()).unwrap());
}

#[test]
fn typed_words_and_large_in_place_use_one_operation() {
    use embedded_hal_async::spi::SpiBus;
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<256, u16> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    for length in [1, 64, 65, 129] {
        let mut words = vec![0x1234; length];
        let before = stats().submissions;
        let mut future = Box::pin(SpiBus::transfer_in_place(&mut spi, &mut words));
        assert!(poll(&mut future).is_pending());
        assert_eq!(unsafe { async_spi_probe_last_bytes() }, length * 2);
        assert_eq!(unsafe { async_spi_probe_last_words() }, length);
        finish(&mut future, 0);
        drop(future);
        assert_eq!(words, vec![0x1234; length]);
        assert_eq!(stats().submissions, before + 1);
    }
    let write = [0x1234u16, 0x5678];
    let mut read = [0xffff; 4];
    let mut future = Box::pin(SpiBus::transfer(&mut spi, &mut read, &write));
    assert!(poll(&mut future).is_pending());
    finish(&mut future, 0);
    drop(future);
    assert_eq!(read, [0x1234, 0x5678, 0, 0]);
    assert_eq!(unsafe { async_spi_probe_tx_tail() }, 0);
}

#[test]
fn capacity_zero_and_failure_preserve_retired_orphan_error() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<2> = AsyncSpiStorage::new();
    static EMPTY: AsyncSpiStorage<0> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    let mut old = Box::pin(spi.write(None, &[1]));
    assert!(poll(&mut old).is_pending());
    drop(old);
    unsafe { async_spi_probe_complete(-29) };
    let before = stats();
    let mut oversized = Box::pin(spi.write(None, &[1, 2, 3]));
    assert_eq!(poll(&mut oversized), Poll::Ready(Err(Error::NoSpace)));
    drop(oversized);
    assert_eq!(stats().submissions, before.submissions);
    assert_eq!(stats().attempts, before.attempts);
    let mut flush = Box::pin(spi.flush());
    assert_eq!(poll(&mut flush), Poll::Ready(Err(Error::Io)));
    drop(flush);
    let mut empty = EMPTY.init(Config::default()).unwrap();
    let mut nothing = Box::pin(empty.write(None, &[]));
    assert_eq!(poll(&mut nothing), Poll::Ready(Ok(())));
    drop(nothing);
    let mut something = Box::pin(empty.write(None, &[1]));
    assert_eq!(poll(&mut something), Poll::Ready(Err(Error::NoSpace)));
}

#[test]
fn error_rejection_inline_completion_and_reuse_balance_resources() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    let mut rx = [0xcc; 4];
    let mut future = Box::pin(spi.read(None, &mut rx));
    assert!(poll(&mut future).is_pending());
    finish(&mut future, -29);
    drop(future);
    assert_eq!(rx, [0xcc; 4], "error copied partially written native RX");
    unsafe { async_spi_probe_mode(2) };
    let cs = ChipSelect::new(2, 7, true);
    let before = stats();
    let mut rejected = Box::pin(spi.write(Some(&cs), &[1]));
    assert_eq!(poll(&mut rejected), Poll::Ready(Err(Error::Io)));
    drop(rejected);
    assert_eq!(stats().completions, before.completions);
    assert_eq!(stats().asserted, before.asserted + 1);
    assert_eq!(stats().deasserted, before.deasserted + 1);
    unsafe { async_spi_probe_mode(1) };
    let mut immediate = Box::pin(spi.write(None, &[2]));
    assert_eq!(poll(&mut immediate), Poll::Ready(Ok(())));
    assert_eq!(poll(&mut immediate), Poll::Ready(Ok(())));
}

#[test]
fn preparation_and_native_init_failures_release_claim() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    unsafe { async_spi_probe_prepare_error(-5) };
    assert!(matches!(
        STORAGE.init(Config::default()),
        Err(Error::NotSupported)
    ));
    assert_eq!(stats().opens, 0);
    assert_eq!(stats().attempts, 0);
    unsafe {
        async_spi_probe_prepare_error(0);
        async_spi_probe_open_error(-29);
    }
    assert!(matches!(STORAGE.init(Config::default()), Err(Error::Io)));
    unsafe { async_spi_probe_open_error(0) };
    drop(STORAGE.init(Config::default()).unwrap());
}

#[cfg(not(zero_heap))]
#[test]
fn heap_header_growth_failure_and_many_forget_cycles_reclaim() {
    let _fixture = Fixture::new();
    unsafe { async_spi_probe_fail_alloc(0) };
    assert!(matches!(
        ove::AsyncSpi::new(Config::default()),
        Err(Error::NoMemory)
    ));
    unsafe { async_spi_probe_fail_alloc(-1) };
    let mut spi = ove::AsyncSpi::new(Config::default()).unwrap();
    let mut first = Box::pin(spi.write(None, &[1, 2]));
    assert!(poll(&mut first).is_pending());
    drop(first);
    unsafe {
        async_spi_probe_complete(-29);
        async_spi_probe_fail_alloc(0);
    }
    let old = stats();
    let mut grow = Box::pin(spi.write(None, &[3; 257]));
    assert_eq!(poll(&mut grow), Poll::Ready(Err(Error::NoMemory)));
    drop(grow);
    assert_eq!(stats().live, old.live);
    assert_eq!(stats().bytes, old.bytes);
    unsafe { async_spi_probe_fail_alloc(-1) };
    let mut flush = Box::pin(spi.flush());
    assert_eq!(poll(&mut flush), Poll::Ready(Err(Error::Io)));
    drop(flush);
    for _ in 0..24 {
        let mut future = spi.write(None, &[4; 257]);
        assert!(poll_stack(&mut future).is_pending());
        std::mem::forget(future);
        unsafe { async_spi_probe_complete(0) };
    }
    assert_eq!(
        stats().live,
        2,
        "forgotten futures leaked operation references"
    );
    assert_eq!(
        stats().allocations,
        3,
        "completed operations failed to reuse capacity"
    );
}

#[cfg(not(zero_heap))]
#[test]
fn native_limit_preflight_precedes_payload_allocation() {
    let _fixture = Fixture::new();
    let mut spi = ove::AsyncSpi16::new(Config::default()).unwrap();
    unsafe { async_spi_probe_limit(4) };
    let before = stats();
    let mut future = Box::pin(spi.write(None, &[1; 5]));
    assert_eq!(poll(&mut future), Poll::Ready(Err(Error::InvalidParam)));
    assert_eq!(stats().attempts, before.attempts);
    assert_eq!(stats().submissions, before.submissions);
}

struct HeldWake {
    gate: Mutex<(bool, bool)>,
    changed: Condvar,
    irq_depth: AtomicUsize,
}
impl HeldWake {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            gate: Mutex::new((false, false)),
            changed: Condvar::new(),
            irq_depth: AtomicUsize::new(usize::MAX),
        })
    }
    fn entered(&self) {
        let mut gate = self.gate.lock().unwrap();
        while !gate.0 {
            gate = self.changed.wait(gate).unwrap();
        }
    }
    fn release(&self) {
        self.gate.lock().unwrap().1 = true;
        self.changed.notify_all();
    }
}
impl Wake for HeldWake {
    fn wake(self: Arc<Self>) {
        self.irq_depth.store(
            unsafe { async_spi_probe_irq_depth() } as usize,
            Ordering::Release,
        );
        let mut gate = self.gate.lock().unwrap();
        gate.0 = true;
        self.changed.notify_all();
        while !gate.1 {
            gate = self.changed.wait(gate).unwrap();
        }
    }
}

#[test]
fn ready_poll_and_owner_drop_wait_for_callback_tail_before_static_reinit() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    let held = HeldWake::new();
    let waker = Waker::from(held.clone());
    let mut rx = [0; 2];
    let mut future = Box::pin(spi.transfer(None, &[1, 2], &mut rx));
    assert!(
        future
            .as_mut()
            .poll(&mut Context::from_waker(&waker))
            .is_pending()
    );
    let worker = std::thread::spawn(|| unsafe { async_spi_probe_complete(0) });
    held.entered();
    let result = poll(&mut future);
    drop(future);
    drop(spi);
    let before_tail = stats();
    let attempted_reinit = STORAGE.init(Config::default());
    let busy = matches!(&attempted_reinit, Err(Error::Busy));
    // Always release/join before checking a negative observation.
    held.release();
    worker.join().unwrap();
    drop(attempted_reinit);
    assert_eq!(result, Poll::Ready(Ok(())));
    assert_eq!(rx, [1, 2]);
    assert_eq!(
        before_tail.closes, 0,
        "Ready was mistaken for callback-tail retirement"
    );
    assert!(
        busy,
        "static storage reused while its old wake tail remained"
    );
    assert_eq!(
        held.irq_depth.load(Ordering::Acquire),
        0,
        "Waker ran with IRQ masking held"
    );
    drop(STORAGE.init(Config::default()).unwrap());
}

#[cfg(not(zero_heap))]
#[test]
fn heap_backing_survives_ready_owner_drop_until_wake_returns() {
    let _fixture = Fixture::new();
    let mut spi = ove::AsyncSpi::new(Config::default()).unwrap();
    let held = HeldWake::new();
    let waker = Waker::from(held.clone());
    let mut future = Box::pin(spi.write(None, &[1, 2]));
    assert!(
        future
            .as_mut()
            .poll(&mut Context::from_waker(&waker))
            .is_pending()
    );
    let worker = std::thread::spawn(|| unsafe { async_spi_probe_complete(0) });
    held.entered();
    let result = poll(&mut future);
    drop(future);
    drop(spi);
    let before_tail = stats();
    held.release();
    worker.join().unwrap();
    assert_eq!(result, Poll::Ready(Ok(())));
    assert_eq!(
        before_tail.live, 2,
        "native backing freed while callback wake remained"
    );
    assert_eq!(before_tail.closes, 0);
    assert_eq!(
        stats().live,
        0,
        "final callback tail failed to reclaim native allocations"
    );
}

type LocalTask = Pin<Box<dyn Future<Output = ()>>>;
thread_local! {
    static TASK: RefCell<Option<LocalTask>> = const { RefCell::new(None) };
}

struct LocalWake {
    thread: std::thread::ThreadId,
    requested: AtomicBool,
    wakes: AtomicUsize,
}
impl LocalWake {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            thread: std::thread::current().id(),
            requested: AtomicBool::new(false),
            wakes: AtomicUsize::new(0),
        })
    }
    fn drive(self: &Arc<Self>) {
        self.requested.store(true, Ordering::Release);
        if self.thread != std::thread::current().id() {
            return;
        }
        loop {
            let Some(mut task) = TASK.with(|slot| slot.borrow_mut().take()) else {
                return;
            };
            self.requested.store(false, Ordering::Release);
            let waker = Waker::from(self.clone());
            let pending = task
                .as_mut()
                .poll(&mut Context::from_waker(&waker))
                .is_pending();
            if pending {
                TASK.with(|slot| *slot.borrow_mut() = Some(task));
            } else {
                drop(task);
            }
            if !pending || !self.requested.swap(false, Ordering::AcqRel) {
                return;
            }
        }
    }
}
impl Wake for LocalWake {
    fn wake(self: Arc<Self>) {
        assert_eq!(
            unsafe { async_spi_probe_irq_depth() },
            0,
            "Waker invoked inside IRQ-masked metadata lock"
        );
        self.wakes.fetch_add(1, Ordering::Relaxed);
        self.drive();
    }
}

fn install(task: impl Future<Output = ()> + 'static) {
    TASK.with(|slot| {
        let mut slot = slot.borrow_mut();
        assert!(slot.is_none());
        *slot = Some(Box::pin(task));
    });
}
fn task_pending() -> bool {
    TASK.with(|slot| slot.borrow().is_some())
}
fn drop_task() {
    TASK.with(|slot| drop(slot.borrow_mut().take()));
}

#[test]
fn reentrant_wake_polls_ready_and_drops_owner_without_early_reinit() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let owner = STORAGE.init(Config::default()).unwrap();
    let observed_busy = Arc::new(AtomicBool::new(false));
    let observed = observed_busy.clone();
    install(async move {
        let mut owner = owner;
        let mut rx = [0; 2];
        owner.transfer(None, &[7, 8], &mut rx).await.unwrap();
        assert_eq!(rx, [7, 8]);
        drop(owner);
        let attempted = STORAGE.init(Config::default());
        observed.store(matches!(&attempted, Err(Error::Busy)), Ordering::Release);
        drop(attempted);
    });
    let executor = LocalWake::new();
    executor.drive();
    assert!(task_pending());
    unsafe { async_spi_probe_complete(0) };
    let finished = !task_pending();
    drop_task();
    assert!(finished, "completion did not drive the suspended task");
    assert!(
        observed_busy.load(Ordering::Acquire),
        "reentrant Ready released static backing before wake tail"
    );
    assert_eq!(executor.wakes.load(Ordering::Acquire), 1);
    drop(STORAGE.init(Config::default()).unwrap());
}

#[test]
fn reentrant_wake_starts_next_generation_without_old_record_writeback() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<32> = AsyncSpiStorage::new();
    let owner = STORAGE.init(Config::default()).unwrap();
    let completed = Arc::new(AtomicBool::new(false));
    let result = completed.clone();
    install(async move {
        let mut owner = owner;
        owner.write(None, &[1]).await.unwrap();
        let mut rx = [0; 17];
        owner.transfer(None, &[9; 17], &mut rx).await.unwrap();
        assert_eq!(rx, [9; 17]);
        result.store(true, Ordering::Release);
    });
    let executor = LocalWake::new();
    executor.drive();
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(stats().submissions, 2);
    // Poll between completions: a stale callback reset must not make the new
    // generation appear Idle, Ready, or already consumed.
    executor.drive();
    assert!(task_pending());
    unsafe { async_spi_probe_complete(0) };
    executor.drive();
    let finished = completed.load(Ordering::Acquire) && !task_pending();
    drop_task();
    assert!(
        finished,
        "old callback disturbed a reentrant new generation"
    );
    assert_eq!(executor.wakes.load(Ordering::Acquire), 2);
}

struct ForwardWake {
    executor: Arc<LocalWake>,
    wakes: AtomicUsize,
}
impl Wake for ForwardWake {
    fn wake(self: Arc<Self>) {
        self.wakes.fetch_add(1, Ordering::AcqRel);
        self.executor.drive();
    }
}

#[test]
fn reentrant_next_generation_keeps_its_distinct_waiter() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<32> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let executor = LocalWake::new();
    let forwarded = Arc::new(ForwardWake {
        executor: executor.clone(),
        wakes: AtomicUsize::new(0),
    });
    let waker = Waker::from(forwarded.clone());
    install(async move {
        owner.write(None, &[1]).await.unwrap();
        let mut next = owner.write(None, &[2; 17]);
        std::future::poll_fn(|_| Pin::new(&mut next).poll(&mut Context::from_waker(&waker)))
            .await
            .unwrap();
    });
    executor.drive();
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(stats().submissions, 2);
    unsafe { async_spi_probe_complete(0) };
    let finished = !task_pending();
    drop_task();
    assert!(finished, "new generation did not wake its task");
    assert_eq!(executor.wakes.load(Ordering::Acquire), 1);
    assert_eq!(
        forwarded.wakes.load(Ordering::Acquire),
        1,
        "old callback replaced the new waiter"
    );
}

#[test]
fn reentrant_inline_next_generation_finishes_before_old_wake_returns() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<32> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    install(async move {
        owner.write(None, &[1]).await.unwrap();
        unsafe { async_spi_probe_mode(1) };
        let mut rx = [0; 17];
        owner.transfer(None, &[9; 17], &mut rx).await.unwrap();
        assert_eq!(rx, [9; 17]);
    });
    let executor = LocalWake::new();
    executor.drive();
    unsafe { async_spi_probe_complete(0) };
    let finished = !task_pending();
    drop_task();
    assert!(
        finished,
        "nested inline callback left a lost result or waiter"
    );
    assert_eq!(stats().submissions, 2);
    assert_eq!(stats().completions, 2);
    drop(STORAGE.init(Config::default()).unwrap());
}

#[cfg(not(zero_heap))]
#[test]
fn reentrant_heap_growth_reclaims_old_pair_while_wake_tail_keeps_header() {
    let _fixture = Fixture::new();
    let mut owner = ove::AsyncSpi::new(Config::default()).unwrap();
    install(async move {
        owner.write(None, &[1]).await.unwrap();
        let mut rx = [0; 257];
        owner.transfer(None, &[9; 257], &mut rx).await.unwrap();
        assert_eq!(rx, [9; 257]);
    });
    let executor = LocalWake::new();
    executor.drive();
    unsafe { async_spi_probe_complete(0) };
    let grown = stats();
    assert_eq!(grown.submissions, 2);
    assert_eq!(grown.allocations, 3);
    assert_eq!(
        grown.frees, 1,
        "old payload retained unnecessarily after final native access"
    );
    assert_eq!(grown.live, 2);
    unsafe { async_spi_probe_complete(0) };
    let finished = !task_pending();
    drop_task();
    assert!(finished);
    assert_eq!(stats().live, 0);
}

#[test]
fn static_word_capacity_admits_exact_boundary_and_rejects_next_word() {
    let _fixture = Fixture::new();
    static BYTES: AsyncSpiStorage<4> = AsyncSpiStorage::new();
    static WORDS: AsyncSpiStorage<4, u16> = AsyncSpiStorage::new();
    static EMPTY: AsyncSpiStorage<0, u16> = AsyncSpiStorage::new();
    let mut bytes = BYTES.init(Config::default()).unwrap();
    let mut future = bytes.write(None, &[1; 4]);
    assert!(poll_stack(&mut future).is_pending());
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(poll_stack(&mut future), Poll::Ready(Ok(())));
    drop(future);
    let before = stats();
    assert_eq!(
        poll_stack(&mut bytes.write(None, &[1; 5])),
        Poll::Ready(Err(Error::NoSpace))
    );
    let mut words = WORDS.init(Config::default()).unwrap();
    let mut future = words.write(None, &[0x1234; 4]);
    assert!(poll_stack(&mut future).is_pending());
    assert_eq!(unsafe { async_spi_probe_last_bytes() }, 8);
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(poll_stack(&mut future), Poll::Ready(Ok(())));
    drop(future);
    assert_eq!(
        poll_stack(&mut words.write(None, &[1; 5])),
        Poll::Ready(Err(Error::NoSpace))
    );
    assert_eq!(stats().submissions, before.submissions + 1);
    let mut empty = EMPTY.init(Config::default()).unwrap();
    assert_eq!(poll_stack(&mut empty.write(None, &[])), Poll::Ready(Ok(())));
    assert_eq!(
        poll_stack(&mut empty.write(None, &[1])),
        Poll::Ready(Err(Error::NoSpace))
    );
}

#[cfg(not(zero_heap))]
#[test]
fn native_frame_limit_and_rejected_growth_preserve_orphan_result() {
    let _fixture = Fixture::new();
    let mut owner = ove::AsyncSpi16::new(Config::default()).unwrap();
    unsafe { async_spi_probe_limit(65535) };
    let data = vec![0x1234; 65535];
    let mut future = owner.write(None, &data);
    assert!(poll_stack(&mut future).is_pending());
    assert_eq!(unsafe { async_spi_probe_last_bytes() }, 131070);
    assert_eq!(unsafe { async_spi_probe_last_words() }, 65535);
    drop(future);
    unsafe { async_spi_probe_complete(-29) };
    let before = stats();
    let oversized = vec![0x1234; 65536];
    assert_eq!(
        poll_stack(&mut owner.write(None, &oversized)),
        Poll::Ready(Err(Error::InvalidParam))
    );
    assert_eq!(stats().attempts, before.attempts);
    assert_eq!(stats().submissions, before.submissions);
    assert_eq!(poll_stack(&mut owner.flush()), Poll::Ready(Err(Error::Io)));
}

#[test]
fn local_irq_model_and_native_worker_serialize_metadata_across_threads() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut spi = STORAGE.init(Config::default()).unwrap();
    // This dependency models local masking without POSIX's global IRQ mutex.
    // It exercises the binding's atomic lock, not an actual SMP RTOS kernel.
    unsafe { async_spi_probe_local_irq_model(1) };
    for _ in 0..64 {
        let mut rx = [0; 4];
        let mut future = Box::pin(spi.transfer(None, &[1, 2, 3, 4], &mut rx));
        assert!(poll(&mut future).is_pending());
        let worker = std::thread::spawn(|| unsafe { async_spi_probe_complete(0) });
        let mut result = Poll::Pending;
        for _ in 0..10000 {
            result = poll(&mut future);
            if result.is_ready() {
                break;
            }
            std::thread::yield_now();
        }
        worker.join().unwrap();
        if result.is_pending() {
            result = poll(&mut future);
        }
        drop(future);
        assert_eq!(result, Poll::Ready(Ok(())));
        assert_eq!(rx, [1, 2, 3, 4]);
    }
}

#[test]
fn active_future_is_movable_without_relocating_native_backing() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let mut rx = [0; 4];
    let mut future = owner.transfer(None, &[1, 2, 3, 4], &mut rx);
    assert!(poll_stack(&mut future).is_pending());
    let pointer = unsafe { async_spi_probe_pending_rx() };
    let mut moved = Some(future).take().unwrap();
    assert_eq!(unsafe { async_spi_probe_pending_rx() }, pointer);
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(poll_stack(&mut moved), Poll::Ready(Ok(())));
    drop(moved);
    assert_eq!(rx, [1, 2, 3, 4]);
}

#[test]
fn chip_select_is_copied_when_future_is_created() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let mut cs = ChipSelect::new(2, 7, true);
    let mut future = Box::pin(owner.write(Some(&cs), &[1]));
    // CS is not part of the returned future's borrow, even before first poll.
    cs = ChipSelect::new(9, 7, true);
    assert!(poll(&mut future).is_pending());
    finish(&mut future, 0);
    drop(future);
    assert_eq!(unsafe { async_spi_probe_last_port() }, 2);
    let mut next = Box::pin(owner.write(Some(&cs), &[1]));
    assert!(poll(&mut next).is_pending());
    finish(&mut next, 0);
    drop(next);
    assert_eq!(unsafe { async_spi_probe_last_port() }, 9);
}

#[test]
fn replacing_a_waiter_drops_the_old_waker_outside_metadata_lock() {
    struct ObservedDrop(Arc<AtomicUsize>);
    impl Drop for ObservedDrop {
        fn drop(&mut self) {
            let depth = unsafe { async_spi_probe_irq_depth() } as usize;
            self.0.store(depth, Ordering::Release);
        }
    }
    impl Wake for ObservedDrop {
        fn wake(self: Arc<Self>) {}
    }
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let depth = Arc::new(AtomicUsize::new(usize::MAX));
    let old = Waker::from(Arc::new(ObservedDrop(depth.clone())));
    let mut future = Box::pin(owner.write(None, &[1]));
    assert!(
        future
            .as_mut()
            .poll(&mut Context::from_waker(&old))
            .is_pending()
    );
    drop(old);
    assert!(poll(&mut future).is_pending());
    finish(&mut future, 0);
    drop(future);
    assert_eq!(
        depth.load(Ordering::Acquire),
        0,
        "replaced Waker destructor ran inside metadata/IRQ lock"
    );
}

#[test]
fn whole_task_drop_reclaims_pending_owner_only_after_native_completion() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let owner = STORAGE.init(Config::default()).unwrap();
    install(async move {
        let mut owner = owner;
        owner.write(None, &[1, 2]).await.unwrap();
    });
    let executor = LocalWake::new();
    executor.drive();
    assert!(task_pending());
    drop_task();
    assert_eq!(stats().closes, 0);
    assert!(matches!(STORAGE.init(Config::default()), Err(Error::Busy)));
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(
        executor.wakes.load(Ordering::Acquire),
        0,
        "canceled task's waiter survived detach"
    );
    drop(STORAGE.init(Config::default()).unwrap());
}

#[cfg(not(zero_heap))]
#[test]
fn heap_native_init_failure_and_pending_drop_use_matching_real_frees() {
    let _fixture = Fixture::new();
    unsafe { async_spi_probe_open_error(-29) };
    assert!(matches!(
        ove::AsyncSpi::new(Config::default()),
        Err(Error::Io)
    ));
    assert_eq!(stats().live, 0);
    assert_eq!(stats().mutex_inits, stats().mutex_deinits);
    unsafe { async_spi_probe_open_error(0) };
    let mut owner = ove::AsyncSpi::new(Config::default()).unwrap();
    let mut future = owner.write(None, &[1, 2]);
    assert!(poll_stack(&mut future).is_pending());
    std::mem::forget(future);
    drop(owner);
    assert_eq!(stats().live, 2);
    assert_eq!(stats().closes, 0);
    unsafe { async_spi_probe_complete(0) };
    assert_eq!(stats().live, 0);
    assert_eq!(stats().closes, 1);
}

#[cfg(not(zero_heap))]
#[test]
fn native_aligned_backing_validates_padding_and_matching_free() {
    let _fixture = Fixture::new();
    for alignment in [1, 2, 4, 8, 16, 64, 256, 4096] {
        let pointer = unsafe { ove::ffi::ove_spi_binding_alloc(31, alignment) };
        assert!(!pointer.is_null());
        assert_eq!(pointer as usize % alignment, 0);
        unsafe {
            pointer.cast::<u8>().write_bytes(0xa5, 31);
            ove::ffi::ove_spi_binding_free(pointer);
        }
    }
    let before = stats().attempts;
    for (size, alignment) in [
        (0, 8),
        (1, 0),
        (1, 3),
        (usize::MAX, 8),
        (isize::MAX as usize, 8),
        (1, 1usize << (usize::BITS - 1)),
    ] {
        assert!(unsafe { ove::ffi::ove_spi_binding_alloc(size, alignment) }.is_null());
    }
    assert_eq!(
        stats().attempts,
        before,
        "invalid layout reached native allocation"
    );
    unsafe { async_spi_probe_fail_alloc(0) };
    assert!(unsafe { ove::ffi::ove_spi_binding_alloc(31, 8) }.is_null());
    unsafe {
        ove::ffi::ove_spi_binding_free(std::ptr::null_mut());
        async_spi_probe_fail_alloc(-1);
    }
}

struct ReenterWaker {
    on_clone: AtomicBool,
    on_drop: AtomicBool,
    clone_irq: AtomicUsize,
    drop_irq: AtomicUsize,
}
impl ReenterWaker {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            on_clone: AtomicBool::new(false),
            on_drop: AtomicBool::new(false),
            clone_irq: AtomicUsize::new(usize::MAX),
            drop_irq: AtomicUsize::new(usize::MAX),
        })
    }
    fn waker(self: &Arc<Self>) -> Waker {
        let pointer = Arc::into_raw(self.clone()).cast();
        // SAFETY: every raw slot owns an Arc count. The data contains only
        // atomics, and its optional native dependency action is thread-safe.
        unsafe { Waker::from_raw(RawWaker::new(pointer, &REENTER_VTABLE)) }
    }
}
unsafe fn reenter_clone(pointer: *const ()) -> RawWaker {
    let pointer = pointer.cast::<ReenterWaker>();
    unsafe { Arc::increment_strong_count(pointer) };
    let state = unsafe { &*pointer };
    if state.on_clone.swap(false, Ordering::AcqRel) {
        let depth = unsafe { async_spi_probe_irq_depth() } as usize;
        state.clone_irq.store(depth, Ordering::Release);
        // Record a forbidden lock context without making a negative test
        // deadlock inside completion. Main performs cleanup after polling.
        if depth == 0 {
            unsafe { async_spi_probe_complete(0) };
        }
    }
    RawWaker::new(pointer.cast(), &REENTER_VTABLE)
}
unsafe fn reenter_wake(pointer: *const ()) {
    drop(unsafe { Arc::from_raw(pointer.cast::<ReenterWaker>()) });
}
unsafe fn reenter_wake_by_ref(_pointer: *const ()) {}
unsafe fn reenter_drop(pointer: *const ()) {
    let state = unsafe { Arc::from_raw(pointer.cast::<ReenterWaker>()) };
    if state.on_drop.swap(false, Ordering::AcqRel) {
        let depth = unsafe { async_spi_probe_irq_depth() } as usize;
        state.drop_irq.store(depth, Ordering::Release);
        if depth == 0 {
            unsafe { async_spi_probe_complete(0) };
        }
    }
}
static REENTER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    reenter_clone,
    reenter_wake,
    reenter_wake_by_ref,
    reenter_drop,
);

#[test]
fn completion_between_waker_clone_and_metadata_check_is_not_lost() {
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let mut rx = [0; 2];
    let mut future = Box::pin(owner.transfer(None, &[1, 2], &mut rx));
    assert!(poll(&mut future).is_pending());
    let action = ReenterWaker::new();
    let waker = action.waker();
    action.on_clone.store(true, Ordering::Release);
    let result = future.as_mut().poll(&mut Context::from_waker(&waker));
    if result.is_pending() {
        unsafe { async_spi_probe_complete(0) };
        assert_eq!(poll(&mut future), Poll::Ready(Ok(())));
    }
    drop(future);
    assert_eq!(
        action.clone_irq.load(Ordering::Acquire),
        0,
        "Waker clone ran inside metadata/IRQ lock"
    );
    assert_eq!(
        result,
        Poll::Ready(Ok(())),
        "completion during clone was lost"
    );
    assert_eq!(rx, [1, 2]);
}

#[test]
fn replaced_waker_drop_can_complete_and_wake_the_new_waiter() {
    struct CountWake(AtomicUsize);
    impl Wake for CountWake {
        fn wake(self: Arc<Self>) {
            self.0.fetch_add(1, Ordering::Relaxed);
        }
    }
    let _fixture = Fixture::new();
    static STORAGE: AsyncSpiStorage<8> = AsyncSpiStorage::new();
    let mut owner = STORAGE.init(Config::default()).unwrap();
    let action = ReenterWaker::new();
    let old = action.waker();
    let mut future = Box::pin(owner.write(None, &[1, 2]));
    assert!(
        future
            .as_mut()
            .poll(&mut Context::from_waker(&old))
            .is_pending()
    );
    drop(old);
    action.on_drop.store(true, Ordering::Release);
    let count = Arc::new(CountWake(AtomicUsize::new(0)));
    let next = Waker::from(count.clone());
    let result = future.as_mut().poll(&mut Context::from_waker(&next));
    if stats().completions == 0 {
        unsafe { async_spi_probe_complete(0) };
    }
    assert_eq!(poll(&mut future), Poll::Ready(Ok(())));
    drop(future);
    assert!(result.is_pending());
    assert_eq!(
        action.drop_irq.load(Ordering::Acquire),
        0,
        "Waker drop ran inside metadata/IRQ lock"
    );
    assert_eq!(
        count.0.load(Ordering::Acquire),
        1,
        "replaced waiter overwrote the new waiter"
    );
}
