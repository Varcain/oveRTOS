// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! A local task relay with a thread-safe Waker. Foreign wakes only schedule;
//! actual task access stays on its owning thread with no active poll alias.
use super::{AsyncI2c, AsyncI2cStorage, Config, Error, SERIAL, clean, complete, reset, stats};
use core::{
    cell::UnsafeCell,
    future::Future,
    pin::Pin,
    ptr::NonNull,
    task::{Context, Poll, RawWaker, RawWakerVTable, Waker},
};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread::ThreadId,
};
unsafe extern "C" {
    fn async_i2c_probe_irq_depth() -> u32;
    fn async_i2c_probe_pending_count() -> u32;
}
static PROBE: AsyncI2cStorage<8> = AsyncI2cStorage::new();

struct WakeControl {
    owner: ThreadId,
    task: AtomicUsize,
    enabled: AtomicBool,
    polling: AtomicBool,
    wakes: AtomicUsize,
    clones: AtomicUsize,
    drops: AtomicUsize,
    depth_errors: AtomicUsize,
    panics: AtomicUsize,
    probe_busy: AtomicBool,
    claim_errors: AtomicUsize,
}
impl WakeControl {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            owner: std::thread::current().id(),
            task: AtomicUsize::new(0),
            enabled: AtomicBool::new(false),
            polling: AtomicBool::new(false),
            wakes: AtomicUsize::new(0),
            clones: AtomicUsize::new(0),
            drops: AtomicUsize::new(0),
            depth_errors: AtomicUsize::new(0),
            panics: AtomicUsize::new(0),
            probe_busy: AtomicBool::new(false),
            claim_errors: AtomicUsize::new(0),
        })
    }
    fn hook(&self) {
        if unsafe { async_i2c_probe_irq_depth() } != 0 {
            self.depth_errors.fetch_add(1, Ordering::SeqCst);
        }
        if self.probe_busy.load(Ordering::Acquire) {
            match PROBE.init(Config::default()) {
                Err(Error::Busy) => {}
                other => {
                    self.claim_errors.fetch_add(1, Ordering::SeqCst);
                    drop(other);
                }
            }
        }
    }
    fn wake(this: &Arc<Self>) {
        this.hook();
        this.wakes.fetch_add(1, Ordering::SeqCst);
        if !this.enabled.load(Ordering::Acquire)
            || std::thread::current().id() != this.owner
            || this.polling.load(Ordering::Acquire)
        {
            return;
        }
        // A native callback must not unwind across C. Preserve any harness panic
        // for a named assertion after the callback and cleanup have returned.
        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| drive(this))).is_err() {
            this.panics.fetch_add(1, Ordering::SeqCst);
        }
    }
}
unsafe fn clone_waker(data: *const ()) -> RawWaker {
    // SAFETY: each RawWaker owns one Arc count until wake/drop; clone is borrowed.
    let control = unsafe { &*data.cast::<WakeControl>() };
    control.hook();
    control.clones.fetch_add(1, Ordering::SeqCst);
    unsafe { Arc::increment_strong_count(data.cast::<WakeControl>()) };
    RawWaker::new(data, &VTABLE)
}
unsafe fn wake_waker(data: *const ()) {
    // SAFETY: wake consumes this RawWaker's Arc count exactly once.
    let control = unsafe { Arc::from_raw(data.cast::<WakeControl>()) };
    WakeControl::wake(&control);
}
unsafe fn wake_ref(data: *const ()) {
    // SAFETY: retain a temporary count so the original borrowed Waker stays live.
    unsafe { Arc::increment_strong_count(data.cast::<WakeControl>()) };
    let control = unsafe { Arc::from_raw(data.cast::<WakeControl>()) };
    WakeControl::wake(&control);
}
unsafe fn drop_waker(data: *const ()) {
    // SAFETY: this callback consumes exactly this RawWaker's Arc count.
    let control = unsafe { Arc::from_raw(data.cast::<WakeControl>()) };
    control.hook();
    control.drops.fetch_add(1, Ordering::SeqCst);
}
static VTABLE: RawWakerVTable = RawWakerVTable::new(clone_waker, wake_waker, wake_ref, drop_waker);
fn waker(control: &Arc<WakeControl>) -> Waker {
    // SAFETY: all Waker data is Send+Sync. Local task access is separately
    // thread-checked, enabled only for the guard's lifetime and poll-exclusive.
    unsafe {
        Waker::from_raw(RawWaker::new(
            Arc::into_raw(control.clone()).cast(),
            &VTABLE,
        ))
    }
}
#[derive(Copy, Clone)]
enum Action {
    DropOwner,
    StartNext,
}
struct LocalTask<'a> {
    owner: &'a UnsafeCell<Option<AsyncI2c>>,
    future: Option<Pin<Box<dyn Future<Output = ove::Result<()>> + 'a>>>,
    results: Vec<ove::Result<()>>,
    action: Action,
    closed_in_wake: Option<usize>,
}
struct Driver<'a> {
    control: Arc<WakeControl>,
    task: NonNull<UnsafeCell<LocalTask<'a>>>,
}
struct PollGuard<'a>(&'a AtomicBool);
impl Drop for PollGuard<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}
fn drive(control: &Arc<WakeControl>) -> Poll<ove::Result<()>> {
    assert_eq!(std::thread::current().id(), control.owner);
    if control
        .polling
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return Poll::Pending;
    }
    let _guard = PollGuard(&control.polling);
    // SAFETY: only this owning thread dereferences task; the atomic guard excludes
    // reentrant access. Driver retains the allocation/borrowed cells while enabled.
    let task = unsafe {
        &mut *(*(control.task.load(Ordering::Acquire) as *mut UnsafeCell<LocalTask<'_>>)).get()
    };
    let Some(mut future) = task.future.take() else {
        return Poll::Ready(Ok(()));
    };
    let waiter = waker(control);
    let result = future.as_mut().poll(&mut Context::from_waker(&waiter));
    if result.is_pending() {
        task.future = Some(future);
        return result;
    }
    // End the operation's exclusive owner borrow before touching its owner cell.
    drop(future);
    let Poll::Ready(result) = result else {
        unreachable!()
    };
    task.results.push(result);
    match task.action {
        Action::DropOwner => {
            // SAFETY: the sole operation was dropped above. No active Future borrow or
            // owner reference remains; the cell is only accessed by this local driver.
            drop(unsafe { (&mut *task.owner.get()).take() });
            task.closed_in_wake = Some(stats().closes);
        }
        Action::StartNext => {
            task.action = Action::DropOwner;
            // SAFETY: the old Future is gone; this is a new exclusive borrow lasting
            // only until the new stored operation is dropped by drive or Driver::drop.
            let owner = unsafe { (&mut *task.owner.get()).as_mut().unwrap() };
            let mut next = Box::pin(owner.write(0x42, &[9; 33]));
            let pending = next.as_mut().poll(&mut Context::from_waker(&waiter));
            assert_eq!(pending, Poll::Pending);
            task.future = Some(next);
        }
    }
    Poll::Ready(result)
}
impl<'a> Driver<'a> {
    fn new(
        owner: &'a UnsafeCell<Option<AsyncI2c>>,
        rx: &'a UnsafeCell<[u8; 7]>,
        action: Action,
    ) -> Self {
        let control = WakeControl::new();
        // SAFETY: these private cells are exclusively driven by Driver. The Future
        // is destroyed before either cell can be read/mutated or leave its scope.
        let future = unsafe {
            (&mut *owner.get())
                .as_mut()
                .unwrap()
                .write_read(0x42, &[1, 2, 3], &mut *rx.get())
        };
        let task = Box::new(UnsafeCell::new(LocalTask {
            owner,
            future: Some(Box::pin(future)),
            results: Vec::new(),
            action,
            closed_in_wake: None,
        }));
        // Raw ownership avoids invalidating a cached task pointer through later Box
        // borrows. All accesses use the same UnsafeCell; Drop recovers this one Box.
        let task = NonNull::new(Box::into_raw(task)).unwrap();
        control
            .task
            .store(task.as_ptr() as usize, Ordering::Release);
        control.enabled.store(true, Ordering::Release);
        Self { control, task }
    }
    fn poll(&self) -> Poll<ove::Result<()>> {
        drive(&self.control)
    }
    fn results(&self) -> Vec<ove::Result<()>> {
        assert!(!self.control.polling.load(Ordering::Acquire));
        // SAFETY: native foreign wakes do not access this task; no local drive is active.
        unsafe { (&*self.task.as_ref().get()).results.clone() }
    }
    fn closed_in_wake(&self) -> Option<usize> {
        assert!(!self.control.polling.load(Ordering::Acquire));
        unsafe { (&*self.task.as_ref().get()).closed_in_wake }
    }
}
impl Drop for Driver<'_> {
    fn drop(&mut self) {
        self.control.enabled.store(false, Ordering::Release);
        // SAFETY: only the owning thread ever accesses the task. Disabled/foreign
        // wakes cannot dereference it. End its Future borrow before owner cleanup.
        unsafe {
            let task = &mut *self.task.as_ref().get();
            drop(task.future.take());
            drop((&mut *task.owner.get()).take());
            while async_i2c_probe_pending_count() != 0 {
                super::async_i2c_probe_complete(0);
            }
            drop(Box::from_raw(self.task.as_ptr()));
        }
    }
}
#[test]
fn wake_can_finish_drop_and_reclaim_only_after_its_tail() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    let owner = UnsafeCell::new(Some(super::bus()));
    let rx = UnsafeCell::new([0xcc; 7]);
    let driver = Driver::new(&owner, &rx, Action::DropOwner);
    assert_eq!(driver.poll(), Poll::Pending);
    let foreign = waker(&driver.control);
    std::thread::spawn(move || foreign.wake()).join().unwrap();
    assert!(driver.results().is_empty());
    complete(Ok(()));
    let panics = driver.control.panics.load(Ordering::SeqCst);
    let closed = driver.closed_in_wake();
    let results = driver.results();
    drop(driver);
    assert_eq!(panics, 0);
    assert_eq!(results, vec![Ok(())]);
    assert_eq!(closed, Some(0));
    assert_eq!(stats().closes, 1);
    // SAFETY: Driver ended the sole RX borrow before being dropped.
    assert_eq!(unsafe { *rx.get() }, [0x5a; 7]);
    clean();
}
#[test]
fn old_wake_tail_cannot_overwrite_new_generation_or_growth() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    let owner = UnsafeCell::new(Some(super::bus()));
    let rx = UnsafeCell::new([0xcc; 7]);
    let driver = Driver::new(&owner, &rx, Action::StartNext);
    assert_eq!(driver.poll(), Poll::Pending);
    complete(Ok(()));
    assert_eq!(driver.results(), vec![Ok(())]);
    assert_eq!(stats().submissions, 2);
    assert_eq!(driver.poll(), Poll::Pending);
    complete(Err(Error::Io));
    let results = driver.results();
    let panics = driver.control.panics.load(Ordering::SeqCst);
    let closed = driver.closed_in_wake();
    drop(driver);
    assert_eq!(panics, 0);
    assert_eq!(results, vec![Ok(()), Err(Error::Io)]);
    assert_eq!(closed, Some(0));
    assert_eq!(unsafe { super::async_i2c_probe_tx_len() }, 33);
    assert_eq!(unsafe { super::async_i2c_probe_rx_len() }, 0);
    assert_eq!(unsafe { super::async_i2c_probe_tx_tail() }, 9);
    clean();
}
#[test]
fn waker_clone_replace_detach_wake_and_close_run_outside_irq_lock() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    let mut owner = PROBE.init(Config::default()).unwrap();
    let first = WakeControl::new();
    let second = WakeControl::new();
    first.probe_busy.store(true, Ordering::Release);
    second.probe_busy.store(true, Ordering::Release);
    let one = waker(&first);
    let two = waker(&second);
    let mut future = owner.write(0x42, &[1]);
    assert_eq!(
        Pin::new(&mut future).poll(&mut Context::from_waker(&one)),
        Poll::Pending
    );
    assert_eq!(
        Pin::new(&mut future).poll(&mut Context::from_waker(&two)),
        Poll::Pending
    );
    drop(future);
    complete(Ok(()));
    let mut future = owner.write(0x42, &[2]);
    assert_eq!(
        Pin::new(&mut future).poll(&mut Context::from_waker(&one)),
        Poll::Pending
    );
    complete(Ok(()));
    assert_eq!(super::poll(&mut future), Poll::Ready(Ok(())));
    drop(future);
    let mut future = owner.write(0x42, &[3]);
    assert_eq!(
        Pin::new(&mut future).poll(&mut Context::from_waker(&two)),
        Poll::Pending
    );
    core::mem::forget(future);
    drop(owner);
    complete(Ok(()));
    first.probe_busy.store(false, Ordering::Release);
    second.probe_busy.store(false, Ordering::Release);
    drop(one);
    drop(two);
    for control in [first, second] {
        assert_eq!(control.depth_errors.load(Ordering::SeqCst), 0);
        assert_eq!(control.claim_errors.load(Ordering::SeqCst), 0);
        assert!(control.clones.load(Ordering::SeqCst) > 0);
        assert!(control.drops.load(Ordering::SeqCst) > 0);
    }
    clean();
}
