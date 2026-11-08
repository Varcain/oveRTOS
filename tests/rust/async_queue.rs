// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

use ove::{AsyncQueue, Error, Queue, QueueStorage, Result};
use static_cell::StaticCell;
use std::future::Future;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::task::{Context, Poll, Wake, Waker};

static CHECKS: AtomicU32 = AtomicU32::new(0);
static FAILURES: AtomicU32 = AtomicU32::new(0);
static STARTED: AtomicU32 = AtomicU32::new(0);
static FINISHED: AtomicU32 = AtomicU32::new(0);
static PRIMARY: QueueStorage<u32, 2> = QueueStorage::new();
static ORDINARY: QueueStorage<u32, 2> = QueueStorage::new();
// Permanent placement roots the native owner and callback data. The host's
// standard-library Arc/Wake and worker support still use its Rust allocator.
static WRAPPER: StaticCell<AsyncQueue<u32, 2>> = StaticCell::new();

fn check(value: bool, name: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !value {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        println!("FAIL: {name}");
    }
}

fn queue(storage: &'static QueueStorage<u32, 2>) -> Result<Queue<u32, 2>> {
    Queue::create(storage)
}

fn empty(q: &Queue<u32, 2>) -> bool {
    matches!(
        q.try_recv(),
        Err(Error::QueueEmpty | Error::WouldBlock | Error::Timeout)
    )
}

struct WakeCount(AtomicU32);

impl Wake for WakeCount {
    fn wake(self: Arc<Self>) {
        self.0.fetch_add(1, Ordering::Release);
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.0.fetch_add(1, Ordering::Release);
    }
}

fn main() -> std::process::ExitCode {
    let canary = match std::env::args().nth(1).as_deref() {
        None => false,
        Some("canary") => true,
        _ => return std::process::ExitCode::from(2),
    };
    if canary {
        check(false, "early_calibration_survives_controls");
    }
    let Ok(inner) = queue(&PRIMARY) else {
        eprintln!("queue setup failed");
        return std::process::ExitCode::from(2);
    };
    let storage = WRAPPER.init(AsyncQueue::new(inner));
    let Ok(ready) = storage.arm() else {
        eprintln!("async setup failed");
        return std::process::ExitCode::from(2);
    };
    let count = Arc::new(WakeCount(AtomicU32::new(0)));
    let waker = Waker::from(Arc::clone(&count));
    let mut context = Context::from_waker(&waker);
    {
        let mut future = std::pin::pin!(ready.recv());
        check(
            future.as_mut().poll(&mut context).is_pending(),
            "first_pending",
        );
        check(count.0.load(Ordering::Acquire) == 0, "no_wake_before_post");
        check(ready.inner().try_send(&11).is_ok(), "first_sent");
        check(count.0.load(Ordering::Acquire) == 1, "first_wake");
        check(
            matches!(future.as_mut().poll(&mut context), Poll::Ready(Ok(11))),
            "first_ready",
        );
        check(empty(ready.inner()), "first_consumed");
    }
    {
        let mut future = std::pin::pin!(ready.recv());
        check(
            future.as_mut().poll(&mut context).is_pending(),
            "second_pending",
        );
        let worker = std::thread::spawn(move || {
            STARTED.fetch_add(1, Ordering::Release);
            let result = ready.inner().try_send(&22);
            FINISHED.fetch_add(1, Ordering::Release);
            result
        });
        check(
            matches!(worker.join(), Ok(Ok(()))),
            "worker_joined_and_sent",
        );
        check(
            STARTED.load(Ordering::Acquire) == 1 && FINISHED.load(Ordering::Acquire) == 1,
            "worker_completed",
        );
        check(count.0.load(Ordering::Acquire) == 2, "second_wake");
        check(
            matches!(future.as_mut().poll(&mut context), Poll::Ready(Ok(22))),
            "second_ready",
        );
        check(empty(ready.inner()), "second_consumed");
    }
    let (was_empty, fifo) = queue(&ORDINARY).map_or((false, false), |owner| {
        let was_empty = empty(&owner);
        let fifo = owner.try_send(&31).is_ok()
            && owner.try_send(&47).is_ok()
            && owner.try_recv() == Ok(31)
            && owner.try_recv() == Ok(47);
        (was_empty, fifo)
    });
    check(was_empty, "ordinary_empty");
    check(fifo, "ordinary_fifo");
    check(
        queue(&ORDINARY).is_ok_and(|owner| empty(&owner)),
        "ordinary_reused",
    );
    let failures = FAILURES.load(Ordering::Acquire);
    let mode = if cfg!(zero_heap) { "zero" } else { "heap" };
    println!(
        "async queue {mode}: checks={} failures={failures} wakes={} workers={},{}",
        CHECKS.load(Ordering::Acquire),
        count.0.load(Ordering::Acquire),
        STARTED.load(Ordering::Acquire),
        FINISHED.load(Ordering::Acquire)
    );
    std::process::ExitCode::from(u8::from(failures != 0))
}
