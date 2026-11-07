// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

use ove::{AsyncSemaphore, Error, Result, Semaphore, SemaphoreStorage};
use std::future::Future;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, OnceLock};
use std::task::{Context, Poll, Wake, Waker};

static CHECKS: AtomicU32 = AtomicU32::new(0);
static FAILURES: AtomicU32 = AtomicU32::new(0);
static STARTED: AtomicU32 = AtomicU32::new(0);
static FINISHED: AtomicU32 = AtomicU32::new(0);
static PRIMARY: SemaphoreStorage = SemaphoreStorage::new();
static ORDINARY: SemaphoreStorage = SemaphoreStorage::new();
// The API requires a static owner. Keep the host's leaked wrapper genuinely
// rooted for the program lifetime; this host control uses the Rust allocator.
static ROOT: OnceLock<&'static AsyncSemaphore> = OnceLock::new();

fn check(value: bool, name: &str) {
    CHECKS.fetch_add(1, Ordering::Relaxed);
    if !value {
        FAILURES.fetch_add(1, Ordering::Relaxed);
        println!("FAIL: {name}");
    }
}

fn semaphore(storage: &'static SemaphoreStorage) -> Result<Semaphore> {
    #[cfg(zero_heap)]
    {
        Semaphore::create(storage, 0, 1)
    }
    #[cfg(not(zero_heap))]
    {
        let _ = storage;
        Semaphore::new(0, 1)
    }
}

fn empty(sem: &Semaphore) -> bool {
    matches!(sem.try_acquire(), Err(Error::Timeout | Error::WouldBlock))
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
    let Ok(inner) = semaphore(&PRIMARY) else {
        eprintln!("semaphore setup failed");
        return std::process::ExitCode::from(2);
    };
    let storage = Box::leak(Box::new(AsyncSemaphore::new(inner)));
    let Ok(ready) = storage.arm() else {
        eprintln!("async setup failed");
        return std::process::ExitCode::from(2);
    };
    if ROOT.set(ready).is_err() {
        return std::process::ExitCode::from(2);
    }
    let count = Arc::new(WakeCount(AtomicU32::new(0)));
    let waker = Waker::from(Arc::clone(&count));
    let mut context = Context::from_waker(&waker);
    {
        let mut future = std::pin::pin!(ready.acquire());
        check(
            future.as_mut().poll(&mut context).is_pending(),
            "first_pending",
        );
        check(count.0.load(Ordering::Acquire) == 0, "no_wake_before_post");
        ready.inner().release();
        check(count.0.load(Ordering::Acquire) == 1, "first_wake");
        check(
            matches!(future.as_mut().poll(&mut context), Poll::Ready(Ok(()))),
            "first_ready",
        );
        check(empty(ready.inner()), "first_consumed");
    }
    {
        let mut future = std::pin::pin!(ready.acquire());
        check(
            future.as_mut().poll(&mut context).is_pending(),
            "second_pending",
        );
        let worker = std::thread::spawn(move || {
            STARTED.fetch_add(1, Ordering::Release);
            ready.inner().release();
            FINISHED.fetch_add(1, Ordering::Release);
        });
        check(worker.join().is_ok(), "worker_joined");
        check(
            STARTED.load(Ordering::Acquire) == 1 && FINISHED.load(Ordering::Acquire) == 1,
            "worker_completed",
        );
        check(count.0.load(Ordering::Acquire) == 2, "second_wake");
        check(
            matches!(future.as_mut().poll(&mut context), Poll::Ready(Ok(()))),
            "second_ready",
        );
        check(empty(ready.inner()), "second_consumed");
    }
    let (was_empty, consumed) = semaphore(&ORDINARY).map_or((false, false), |owner| {
        let was_empty = empty(&owner);
        owner.release();
        (was_empty, owner.try_acquire().is_ok())
    });
    check(was_empty, "ordinary_empty");
    check(consumed, "ordinary_consumed");
    let reused = semaphore(&ORDINARY).is_ok_and(|owner| empty(&owner));
    check(reused, "ordinary_reused");
    let failures = FAILURES.load(Ordering::Acquire);
    let mode = if cfg!(zero_heap) { "zero" } else { "heap" };
    println!(
        "async semaphore {mode}: checks={} failures={failures} wakes={} workers={},{}",
        CHECKS.load(Ordering::Acquire),
        count.0.load(Ordering::Acquire),
        STARTED.load(Ordering::Acquire),
        FINISHED.load(Ordering::Acquire),
    );
    std::process::ExitCode::from(u8::from(failures != 0))
}
