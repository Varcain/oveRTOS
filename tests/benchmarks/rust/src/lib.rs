// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Benchmark Application (Rust)
//!
//! Measures latency, throughput, and memory usage of all RTOS abstractions
//! using safe Rust bindings. Suite symbols are exported as
//! `#[unsafe(no_mangle)] pub static bench_suite_*` via the
//! [`bench_suite!`](crate::bench_suite) macro so the shared C harness
//! drives them.
//!
//! Bench-specific harness wrappers + macros used to live under
//! `ove::bench` / `bench_case!` etc.; they were extracted here so
//! the binding crate carries no benchmark plumbing.
//!
//! All resource creation uses the unified `ove::*!` macros so the
//! benchmark builds in both heap and zero-heap configurations.

#![cfg_attr(not(feature = "std"), no_std)]
// Bench-app-local: this crate now hosts the C-harness FFI wrapper
// (`mod bench`) plus the `bench_case!` / `bench_suite!` macros, which
// emit `unsafe extern "C"` trampolines.  The user-side bench code
// in this file stays free of explicit `unsafe` blocks; the unsafe
// surface is fenced inside `bench` and the macro-generated trampolines.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

pub mod bench;
mod bench_macros;

use crate::bench::{BenchType, CBenchSuite};
use ove::{
    CondVar, Event, EventGroup, LvCell, Mutex, Priority, Queue, RecursiveMutex, Semaphore, Stream,
    Thread, Timer, Work, Workqueue,
};

// =========================================================================
//  Work handler — safe Rust fn, wrapped into a C trampoline by work_handler!
// =========================================================================

fn wq_work_handler() {
    WQ_WORK_EXECUTED.store(true, core::sync::atomic::Ordering::Relaxed);
    if let Some(sem) = WQ_WORK_SEM.try_get() {
        sem.release();
    }
}

// =========================================================================
//  Suite: time
// =========================================================================

fn time_is_enabled() -> bool {
    true
}

fn time_get_us_overhead_run() {
    // Use the unchecked variant so the bench reflects ove_time_get_us
    // call cost rather than the Result<u64> plumbing of the safe form
    // (the C bench calls the same FFI directly with no error wrap).
    let _ = ove::time::get_us_unchecked();
}
fn delay_1ms_run() {
    ove::time::delay_ms(1);
}

bench_case!(static TIME_GET_US_OVERHEAD: BenchCase = {
    name: b"time_get_us_overhead\0",
    kind: BenchType::Latency,
    run: time_get_us_overhead_run,
    inner_iters: 10,
});

bench_case!(static DELAY_1MS: BenchCase = {
    name: b"delay_1ms\0",
    kind: BenchType::Latency,
    run: delay_1ms_run,
    iterations: 100,
});

bench_suite!(
    symbol = bench_suite_time,
    name = b"time\0",
    enabled = time_is_enabled,
    cases = [TIME_GET_US_OVERHEAD, DELAY_1MS],
);

// =========================================================================
//  Suite: thread
// =========================================================================

ove::shared!(THREAD_BENCH_TH: ove::JoinHandle);
ove::shared!(THREAD_PING_SEM: Semaphore);
ove::shared!(THREAD_PONG_SEM: Semaphore);
static THREAD_CTX_SWITCH_DONE: AtomicBool = AtomicBool::new(false);

fn thread_is_enabled() -> bool {
    true
}

#[cfg(not(zero_heap))]
fn dummy_thread() {}

#[cfg(not(zero_heap))]
fn thread_create_destroy_run() {
    let _th = ove::thread!("bench_tmp", dummy_thread, Priority::Low, 1024);
}

fn thread_yield_run() {
    Thread::yield_now();
}

// Pure "who am I?" query — kernel-side TLS read with no scheduling
// side-effects.  Distinct from time_get_us_overhead and yield.
fn thread_get_self_run() {
    let _ = core::hint::black_box(Thread::current());
}

fn thread_sleep_1ms_run() {
    Thread::sleep_ms(1);
}

fn pong_thread() {
    while !THREAD_CTX_SWITCH_DONE.load(Ordering::Relaxed) {
        if let (Some(ping), Some(pong)) = (THREAD_PING_SEM.try_get(), THREAD_PONG_SEM.try_get()) {
            let _ = ping.acquire();
            pong.release();
        } else {
            // Don't `break` — see comment on contention_thread.  Helper
            // must outlive the bench loop; transient try_get failures
            // are non-fatal.
            Thread::yield_now();
        }
    }
}

fn ctx_switch_setup() {
    THREAD_CTX_SWITCH_DONE.store(false, Ordering::Relaxed);
    THREAD_PING_SEM.init(ove::semaphore!(0, 1));
    THREAD_PONG_SEM.init(ove::semaphore!(0, 1));
    THREAD_BENCH_TH.init(ove::thread!("pong", pong_thread, Priority::Normal, 2048));
}

fn ctx_switch_run() {
    if let (Some(ping), Some(pong)) = (THREAD_PING_SEM.try_get(), THREAD_PONG_SEM.try_get()) {
        ping.release();
        let _ = pong.acquire();
    }
}

fn ctx_switch_teardown() {
    THREAD_CTX_SWITCH_DONE.store(true, Ordering::Relaxed);
    if let Some(ping) = THREAD_PING_SEM.try_get() {
        ping.release();
    }
    Thread::sleep_ms(10);
    THREAD_BENCH_TH.shutdown();
    THREAD_PING_SEM.shutdown();
    THREAD_PONG_SEM.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static THREAD_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: thread_create_destroy_run,
    iterations: 200,
});

bench_case!(static THREAD_YIELD: BenchCase = {
    name: b"yield\0",
    kind: BenchType::Latency,
    run: thread_yield_run,
});

bench_case!(static THREAD_GET_SELF: BenchCase = {
    name: b"get_self\0",
    kind: BenchType::Latency,
    run: thread_get_self_run,
});

bench_case!(static THREAD_SLEEP_1MS: BenchCase = {
    name: b"sleep_1ms\0",
    kind: BenchType::Latency,
    run: thread_sleep_1ms_run,
    iterations: 100,
});

bench_case!(static THREAD_CTX_SWITCH: BenchCase = {
    name: b"ctx_switch\0",
    kind: BenchType::Latency,
    run: ctx_switch_run,
    setup: ctx_switch_setup,
    teardown: ctx_switch_teardown,
    iterations: 500,
});

bench_suite!(
    symbol = bench_suite_thread,
    name = b"thread\0",
    enabled = thread_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        THREAD_CREATE_DESTROY,
        THREAD_YIELD,
        THREAD_GET_SELF,
        THREAD_SLEEP_1MS,
        THREAD_CTX_SWITCH
    ],
);

// =========================================================================
//  Suite: sync
// =========================================================================

ove::shared!(SYNC_MTX: Mutex<()>);
ove::shared!(SYNC_SEM: Semaphore);
ove::shared!(SYNC_EVT: Event);
ove::shared!(SYNC_EVT_ACK: Event);
ove::shared!(SYNC_CV: CondVar);
ove::shared!(SYNC_CV_MTX: Mutex<()>);
ove::shared!(SYNC_RMTX: RecursiveMutex);
ove::shared!(SYNC_CONTENTION_TH: ove::JoinHandle);
ove::shared!(SYNC_EVT_TH: ove::JoinHandle);
ove::shared!(SYNC_CV_TH: ove::JoinHandle);
#[cfg(not(zero_heap))]
ove::shared!(SYNC_MEM_MUTEX: Mutex<()>);
#[cfg(not(zero_heap))]
ove::shared!(SYNC_MEM_SEM: Semaphore);
#[cfg(not(zero_heap))]
ove::shared!(SYNC_MEM_EVENT: Event);
#[cfg(not(zero_heap))]
ove::shared!(SYNC_MEM_CONDVAR: CondVar);

static SYNC_CONTENTION_DONE: AtomicBool = AtomicBool::new(false);
static SYNC_CONTENTION_COUNT: AtomicU32 = AtomicU32::new(0);
static SYNC_EVT_DONE: AtomicBool = AtomicBool::new(false);
static SYNC_CV_DONE: AtomicBool = AtomicBool::new(false);

fn sync_is_enabled() -> bool {
    true
}

// --- Mutex lock/unlock ---
fn mutex_lock_unlock_setup() {
    SYNC_MTX.init(ove::mutex!(()));
}
fn mutex_lock_unlock_run() {
    if let Some(m) = SYNC_MTX.try_get() {
        // Mutex<T> uses RAII: the MutexGuard returned by .lock() drops
        // (and unlocks) at the end of the statement.
        drop(m.lock());
    }
}
fn mutex_lock_unlock_teardown() {
    SYNC_MTX.shutdown();
}

// --- Mutex create/destroy (heap-mode only) ---
#[cfg(not(zero_heap))]
fn mutex_create_destroy_run() {
    let _m = ove::mutex!(());
}

// --- Mutex contention (2-thread throughput) ---
//
// The helper MUST stay alive for the duration of the bench — bailing
// out via `break` on a transient `try_get == None` (e.g. cross-thread
// visibility of SYNC_MTX under Relaxed ordering at thread spawn) makes
// the contention test silently degenerate to single-threaded
// (uncontested) measurement.  Observed empirically on Zephyr where
// timeslicing exposed the race, while FreeRTOS happened not to (the
// runner held the CPU until block).  Setup guarantees SYNC_MTX is
// init'd before this thread is spawned, so the only None-cases are
// "race not yet observed" (busy-wait on yield) or "teardown already
// shutdown the mutex" (DONE flag also set, while-loop exits naturally).
fn contention_thread() {
    while !SYNC_CONTENTION_DONE.load(Ordering::Relaxed) {
        if let Some(m) = SYNC_MTX.try_get() {
            let _g = m.lock();
            SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
            // _g drops at end of scope, releasing the lock.
        } else {
            Thread::yield_now();
        }
    }
}

fn mutex_contention_setup() {
    SYNC_CONTENTION_DONE.store(false, Ordering::Relaxed);
    SYNC_CONTENTION_COUNT.store(0, Ordering::Relaxed);
    SYNC_MTX.init(ove::mutex!(()));
    SYNC_CONTENTION_TH.init(ove::thread!(
        "contention",
        contention_thread,
        Priority::Normal,
        2048
    ));
}

fn mutex_contention_run() {
    if let Some(m) = SYNC_MTX.try_get() {
        let _g = m.lock();
        SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
        // _g drops at end of scope, releasing the lock.
    }
}

fn mutex_contention_teardown() {
    SYNC_CONTENTION_DONE.store(true, Ordering::Relaxed);
    Thread::sleep_ms(10);
    SYNC_CONTENTION_TH.shutdown();
    SYNC_MTX.shutdown();
}

// --- Mutex memory (heap-mode only) ---
#[cfg(not(zero_heap))]
fn mutex_memory_run() {
    SYNC_MEM_MUTEX.try_init(ove::mutex!(())).ok();
}
#[cfg(not(zero_heap))]
fn mutex_memory_teardown() {
    SYNC_MEM_MUTEX.shutdown();
}

// --- Semaphore take/give ---
fn sem_take_give_setup() {
    SYNC_SEM.init(ove::semaphore!(1, 1));
}
fn sem_take_give_run() {
    if let Some(s) = SYNC_SEM.try_get() {
        let _ = s.acquire();
        s.release();
    }
}
fn sem_take_give_teardown() {
    SYNC_SEM.shutdown();
}

// --- Semaphore create/destroy (heap-mode only) ---
#[cfg(not(zero_heap))]
fn sem_create_destroy_run() {
    let _s = ove::semaphore!(0, 1);
}

// --- Semaphore memory (heap-mode only) ---
#[cfg(not(zero_heap))]
fn sem_memory_run() {
    SYNC_MEM_SEM.try_init(ove::semaphore!(0, 1)).ok();
}
#[cfg(not(zero_heap))]
fn sem_memory_teardown() {
    SYNC_MEM_SEM.shutdown();
}

// --- Event signal/wait ---
fn evt_signaler() {
    while !SYNC_EVT_DONE.load(Ordering::Relaxed) {
        if let (Some(e), Some(ack)) = (SYNC_EVT.try_get(), SYNC_EVT_ACK.try_get()) {
            e.signal();
            let _ = ack.wait();
        } else {
            // See comment on contention_thread.
            Thread::yield_now();
        }
    }
}

fn event_signal_wait_setup() {
    SYNC_EVT_DONE.store(false, Ordering::Relaxed);
    SYNC_EVT.init(ove::event!());
    SYNC_EVT_ACK.init(ove::event!());
    SYNC_EVT_TH.init(ove::thread!(
        "evt_sig",
        evt_signaler,
        Priority::Normal,
        1024
    ));
}

fn event_signal_wait_run() {
    if let (Some(e), Some(ack)) = (SYNC_EVT.try_get(), SYNC_EVT_ACK.try_get()) {
        let _ = e.wait();
        ack.signal();
    }
}

fn event_signal_wait_teardown() {
    SYNC_EVT_DONE.store(true, Ordering::Relaxed);
    if let Some(ack) = SYNC_EVT_ACK.try_get() {
        ack.signal();
    }
    Thread::sleep_ms(10);
    SYNC_EVT_TH.shutdown();
    SYNC_EVT.shutdown();
    SYNC_EVT_ACK.shutdown();
}

// --- Event memory (heap-mode only) ---
#[cfg(not(zero_heap))]
fn event_memory_run() {
    SYNC_MEM_EVENT.try_init(ove::event!()).ok();
}
#[cfg(not(zero_heap))]
fn event_memory_teardown() {
    SYNC_MEM_EVENT.shutdown();
}

// --- Condvar signal/wait ---
//
// Condvar uses yield-based signaler + bounded cv_wait timeout — see
// bench_sync.c for why an ack-pattern signaler deadlocks here.
fn cv_signaler() {
    while !SYNC_CV_DONE.load(Ordering::Relaxed) {
        if let Some(cv) = SYNC_CV.try_get() {
            cv.signal();
        } else {
            // See comment on contention_thread.
            // (yield is already in the loop body below — this branch
            // just falls through to the same yield via the next iter.)
        }
        Thread::yield_now();
    }
}

fn condvar_signal_wait_setup() {
    SYNC_CV_DONE.store(false, Ordering::Relaxed);
    SYNC_CV_MTX.init(ove::mutex!(()));
    SYNC_CV.init(ove::condvar!());
    SYNC_CV_TH.init(ove::thread!("cv_sig", cv_signaler, Priority::Normal, 1024));
}

fn condvar_signal_wait_run() {
    if let (Some(mtx), Some(cv)) = (SYNC_CV_MTX.try_get(), SYNC_CV.try_get()) {
        if let Ok(g) = mtx.lock() {
            // Bounded wait: returns Ok((guard, ...)) or Err on backend
            // failure.  Guard re-acquires on return, then drops at end of
            // scope.
            let _ = cv.wait_for(g, core::time::Duration::from_millis(10));
        }
    }
}

fn condvar_signal_wait_teardown() {
    SYNC_CV_DONE.store(true, Ordering::Relaxed);
    if let Some(cv) = SYNC_CV.try_get() {
        cv.signal();
    }
    Thread::sleep_ms(10);
    SYNC_CV_TH.shutdown();
    SYNC_CV.shutdown();
    SYNC_CV_MTX.shutdown();
}

// --- Condvar memory (heap-mode only) ---
#[cfg(not(zero_heap))]
fn condvar_memory_run() {
    SYNC_MEM_CONDVAR.try_init(ove::condvar!()).ok();
}
#[cfg(not(zero_heap))]
fn condvar_memory_teardown() {
    SYNC_MEM_CONDVAR.shutdown();
}

// --- Recursive mutex lock/unlock ---
fn rmtx_lock_unlock_setup() {
    SYNC_RMTX.init(ove::recursive_mutex!());
}
fn rmtx_lock_unlock_run() {
    if let Some(rm) = SYNC_RMTX.try_get() {
        // RAII guard auto-drops at end of statement.
        drop(rm.lock());
    }
}
fn rmtx_lock_unlock_teardown() {
    SYNC_RMTX.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static MUTEX_MEMORY: BenchCase = {
    name: b"mutex_memory\0",
    kind: BenchType::Memory,
    run: mutex_memory_run,
    teardown: mutex_memory_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static SEM_MEMORY: BenchCase = {
    name: b"sem_memory\0",
    kind: BenchType::Memory,
    run: sem_memory_run,
    teardown: sem_memory_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static EVENT_MEMORY: BenchCase = {
    name: b"event_memory\0",
    kind: BenchType::Memory,
    run: event_memory_run,
    teardown: event_memory_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static CONDVAR_MEMORY: BenchCase = {
    name: b"condvar_memory\0",
    kind: BenchType::Memory,
    run: condvar_memory_run,
    teardown: condvar_memory_teardown,
});
bench_case!(static MUTEX_LOCK_UNLOCK: BenchCase = {
    name: b"mutex_lock_unlock\0",
    kind: BenchType::Latency,
    run: mutex_lock_unlock_run,
    setup: mutex_lock_unlock_setup,
    teardown: mutex_lock_unlock_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static MUTEX_CREATE_DESTROY: BenchCase = {
    name: b"mutex_create_destroy\0",
    kind: BenchType::Latency,
    run: mutex_create_destroy_run,
});
bench_case!(static MUTEX_CONTENTION_2T: BenchCase = {
    name: b"mutex_contention_2t\0",
    kind: BenchType::Throughput,
    run: mutex_contention_run,
    setup: mutex_contention_setup,
    teardown: mutex_contention_teardown,
});
bench_case!(static SEM_TAKE_GIVE: BenchCase = {
    name: b"sem_take_give\0",
    kind: BenchType::Latency,
    run: sem_take_give_run,
    setup: sem_take_give_setup,
    teardown: sem_take_give_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static SEM_CREATE_DESTROY: BenchCase = {
    name: b"sem_create_destroy\0",
    kind: BenchType::Latency,
    run: sem_create_destroy_run,
});
bench_case!(static EVENT_SIGNAL_WAIT: BenchCase = {
    name: b"event_signal_wait\0",
    kind: BenchType::Latency,
    run: event_signal_wait_run,
    setup: event_signal_wait_setup,
    teardown: event_signal_wait_teardown,
    iterations: 500,
});
bench_case!(static CONDVAR_SIGNAL_WAIT: BenchCase = {
    name: b"condvar_signal_wait\0",
    kind: BenchType::Latency,
    run: condvar_signal_wait_run,
    setup: condvar_signal_wait_setup,
    teardown: condvar_signal_wait_teardown,
    iterations: 500,
});
bench_case!(static RMTX_LOCK_UNLOCK: BenchCase = {
    name: b"recursive_mutex_lock_unlock\0",
    kind: BenchType::Latency,
    run: rmtx_lock_unlock_run,
    setup: rmtx_lock_unlock_setup,
    teardown: rmtx_lock_unlock_teardown,
});

bench_suite!(
    symbol = bench_suite_sync,
    name = b"sync\0",
    enabled = sync_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        MUTEX_MEMORY,
        #[cfg(not(zero_heap))]
        SEM_MEMORY,
        #[cfg(not(zero_heap))]
        EVENT_MEMORY,
        #[cfg(not(zero_heap))]
        CONDVAR_MEMORY,
        MUTEX_LOCK_UNLOCK,
        #[cfg(not(zero_heap))]
        MUTEX_CREATE_DESTROY,
        MUTEX_CONTENTION_2T,
        SEM_TAKE_GIVE,
        #[cfg(not(zero_heap))]
        SEM_CREATE_DESTROY,
        EVENT_SIGNAL_WAIT,
        CONDVAR_SIGNAL_WAIT,
        RMTX_LOCK_UNLOCK,
    ],
);

// =========================================================================
//  Suite: queue
// =========================================================================

ove::shared!(QUEUE_SEND_RECV_Q: Queue<u32, 16>);
ove::shared!(QUEUE_THROUGHPUT_Q: Queue<u32, 64>);
ove::shared!(QUEUE_PRODUCER_TH: ove::JoinHandle);
#[cfg(not(zero_heap))]
ove::shared!(QUEUE_MEM_Q: Queue<u32, 8>);
static QUEUE_THROUGHPUT_DONE: AtomicBool = AtomicBool::new(false);

fn queue_is_enabled() -> bool {
    true
}

fn queue_send_recv_setup() {
    QUEUE_SEND_RECV_Q.init(ove::queue!(u32, 16));
}
fn queue_send_recv_run() {
    let val: u32 = 42;
    if let Some(q) = QUEUE_SEND_RECV_Q.try_get() {
        let _ = q.send(&val);
        let _ = q.recv();
    }
}
fn queue_send_recv_teardown() {
    QUEUE_SEND_RECV_Q.shutdown();
}

#[cfg(not(zero_heap))]
fn queue_create_destroy_run() {
    let _q = ove::queue!(u32, 8);
}

fn producer_thread() {
    let mut val: u32 = 0;
    while !QUEUE_THROUGHPUT_DONE.load(Ordering::Relaxed) {
        if let Some(q) = QUEUE_THROUGHPUT_Q.try_get() {
            let _ = q.send(&val);
            val = val.wrapping_add(1);
        } else {
            // See comment on contention_thread.
            Thread::yield_now();
        }
    }
}

fn queue_throughput_setup() {
    QUEUE_THROUGHPUT_DONE.store(false, Ordering::Relaxed);
    QUEUE_THROUGHPUT_Q.init(ove::queue!(u32, 64));
    QUEUE_PRODUCER_TH.init(ove::thread!(
        "q_prod",
        producer_thread,
        Priority::Normal,
        2048
    ));
}

fn queue_throughput_run() {
    if let Some(q) = QUEUE_THROUGHPUT_Q.try_get() {
        let _ = q.recv();
    }
}

fn queue_throughput_teardown() {
    QUEUE_THROUGHPUT_DONE.store(true, Ordering::Relaxed);
    if let Some(q) = QUEUE_THROUGHPUT_Q.try_get() {
        let _ = q.try_recv_for(core::time::Duration::from_millis(100));
    }
    Thread::sleep_ms(10);
    QUEUE_PRODUCER_TH.shutdown();
    QUEUE_THROUGHPUT_Q.shutdown();
}

#[cfg(not(zero_heap))]
fn queue_memory_run() {
    QUEUE_MEM_Q.try_init(ove::queue!(u32, 8)).ok();
}
#[cfg(not(zero_heap))]
fn queue_memory_teardown() {
    QUEUE_MEM_Q.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static QUEUE_MEMORY: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: queue_memory_run,
    teardown: queue_memory_teardown,
});
bench_case!(static QUEUE_SEND_RECEIVE: BenchCase = {
    name: b"send_receive\0",
    kind: BenchType::Latency,
    run: queue_send_recv_run,
    setup: queue_send_recv_setup,
    teardown: queue_send_recv_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static QUEUE_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: queue_create_destroy_run,
});
bench_case!(static QUEUE_THROUGHPUT_2T: BenchCase = {
    name: b"throughput_2t\0",
    kind: BenchType::Throughput,
    run: queue_throughput_run,
    setup: queue_throughput_setup,
    teardown: queue_throughput_teardown,
});

bench_suite!(
    symbol = bench_suite_queue,
    name = b"queue\0",
    enabled = queue_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        QUEUE_MEMORY,
        QUEUE_SEND_RECEIVE,
        #[cfg(not(zero_heap))]
        QUEUE_CREATE_DESTROY,
        QUEUE_THROUGHPUT_2T
    ],
);

// =========================================================================
//  Suite: timer
// =========================================================================

ove::shared!(TIMER_TMR: Timer);
#[cfg(not(zero_heap))]
ove::shared!(TIMER_MEM_TMR: Timer);

fn timer_is_enabled() -> bool {
    true
}

fn timer_dummy_cb() {}

#[cfg(not(zero_heap))]
fn timer_create_destroy_run() {
    let _t = ove::timer!(timer_dummy_cb, 1000, false);
}

fn timer_start_stop_setup() {
    TIMER_TMR.init(ove::timer!(timer_dummy_cb, 1000, false));
}
fn timer_start_stop_run() {
    if let Some(t) = TIMER_TMR.try_get() {
        let _ = t.start();
        let _ = t.stop();
    }
}
fn timer_start_stop_teardown() {
    TIMER_TMR.shutdown();
}

#[cfg(not(zero_heap))]
fn timer_memory_run() {
    TIMER_MEM_TMR
        .try_init(ove::timer!(timer_dummy_cb, 1000, false))
        .ok();
}
#[cfg(not(zero_heap))]
fn timer_memory_teardown() {
    TIMER_MEM_TMR.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static TIMER_MEMORY: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: timer_memory_run,
    teardown: timer_memory_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static TIMER_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: timer_create_destroy_run,
});
bench_case!(static TIMER_START_STOP: BenchCase = {
    name: b"start_stop\0",
    kind: BenchType::Latency,
    run: timer_start_stop_run,
    setup: timer_start_stop_setup,
    teardown: timer_start_stop_teardown,
});

bench_suite!(
    symbol = bench_suite_timer,
    name = b"timer\0",
    enabled = timer_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        TIMER_MEMORY,
        #[cfg(not(zero_heap))]
        TIMER_CREATE_DESTROY,
        TIMER_START_STOP
    ],
);

// =========================================================================
//  Suite: eventgroup
// =========================================================================

ove::shared!(EG_BENCH: EventGroup);
#[cfg(not(zero_heap))]
ove::shared!(EG_MEM: EventGroup);

fn eventgroup_is_enabled() -> bool {
    true
}

fn eg_set_get_setup() {
    EG_BENCH.init(ove::eventgroup!());
}
fn eg_set_get_run() {
    if let Some(eg) = EG_BENCH.try_get() {
        eg.set_bits(0x01);
        eg.get_bits();
        eg.clear_bits(0x01);
    }
}
fn eg_set_get_teardown() {
    EG_BENCH.shutdown();
}

#[cfg(not(zero_heap))]
fn eg_create_destroy_run() {
    let _eg = ove::eventgroup!();
}

#[cfg(not(zero_heap))]
fn eg_memory_run() {
    EG_MEM.try_init(ove::eventgroup!()).ok();
}
#[cfg(not(zero_heap))]
fn eg_memory_teardown() {
    EG_MEM.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static EG_MEMORY: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: eg_memory_run,
    teardown: eg_memory_teardown,
});
bench_case!(static EG_SET_GET: BenchCase = {
    name: b"set_get_bits\0",
    kind: BenchType::Latency,
    run: eg_set_get_run,
    setup: eg_set_get_setup,
    teardown: eg_set_get_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static EG_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: eg_create_destroy_run,
});

bench_suite!(
    symbol = bench_suite_eventgroup,
    name = b"eventgroup\0",
    enabled = eventgroup_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        EG_MEMORY,
        EG_SET_GET,
        #[cfg(not(zero_heap))]
        EG_CREATE_DESTROY
    ],
);

// =========================================================================
//  Suite: workqueue
// =========================================================================

ove::shared!(WQ_BENCH: Workqueue);
ove::shared!(WQ_WORK: Work);
ove::shared!(WQ_WORK_SEM: Semaphore);
#[cfg(not(zero_heap))]
ove::shared!(WQ_MEM: Workqueue);
static WQ_WORK_EXECUTED: AtomicBool = AtomicBool::new(false);

fn workqueue_is_enabled() -> bool {
    true
}

#[cfg(not(zero_heap))]
fn wq_create_destroy_run() {
    let _wq = ove::workqueue!(c"bench_wq", Priority::Normal, 2048);
}

fn wq_submit_setup() {
    WQ_WORK_SEM.init(ove::semaphore!(0, 1));
    WQ_BENCH.init(ove::workqueue!(c"bench_wq", Priority::Normal, 2048));
    WQ_WORK.init(ove::work!(ove::work_handler!(wq_work_handler)));
}

fn wq_submit_run() {
    WQ_WORK_EXECUTED.store(false, Ordering::Relaxed);
    if let (Some(w), Some(wq), Some(sem)) =
        (WQ_WORK.try_get(), WQ_BENCH.try_get(), WQ_WORK_SEM.try_get())
    {
        let _ = w.submit(wq);
        let _ = sem.try_acquire_for(core::time::Duration::from_millis(1000));
    }
}

fn wq_submit_teardown() {
    WQ_WORK.shutdown();
    WQ_BENCH.shutdown();
    WQ_WORK_SEM.shutdown();
}

#[cfg(not(zero_heap))]
fn wq_memory_run() {
    WQ_MEM
        .try_init(ove::workqueue!(c"bench_wq", Priority::Normal, 2048))
        .ok();
}
#[cfg(not(zero_heap))]
fn wq_memory_teardown() {
    WQ_MEM.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static WQ_MEMORY_C: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: wq_memory_run,
    teardown: wq_memory_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static WQ_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: wq_create_destroy_run,
    iterations: 200,
});
bench_case!(static WQ_SUBMIT_EXECUTE: BenchCase = {
    name: b"submit_execute\0",
    kind: BenchType::Latency,
    run: wq_submit_run,
    setup: wq_submit_setup,
    teardown: wq_submit_teardown,
    iterations: 500,
});

bench_suite!(
    symbol = bench_suite_workqueue,
    name = b"workqueue\0",
    enabled = workqueue_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        WQ_MEMORY_C,
        #[cfg(not(zero_heap))]
        WQ_CREATE_DESTROY,
        WQ_SUBMIT_EXECUTE
    ],
);

// =========================================================================
//  Suite: stream
// =========================================================================

const STREAM_BUF_SIZE: usize = 256;
const STREAM_MSG_SIZE: usize = 64;

ove::shared!(STREAM_BENCH: Stream<STREAM_BUF_SIZE>);
ove::shared!(STREAM_PRODUCER_TH: ove::JoinHandle);
#[cfg(not(zero_heap))]
ove::shared!(STREAM_MEM: Stream<STREAM_BUF_SIZE>);
ove::shared!(STREAM_BUFS: LvCell<([u8; STREAM_MSG_SIZE], [u8; STREAM_MSG_SIZE])>);
static STREAM_DONE: AtomicBool = AtomicBool::new(false);

fn stream_is_enabled() -> bool {
    true
}

fn stream_send_recv_setup() {
    STREAM_BENCH.init(ove::stream!(STREAM_BUF_SIZE, 1));
    STREAM_BUFS
        .get()
        .set(([0xAA; STREAM_MSG_SIZE], [0u8; STREAM_MSG_SIZE]));
}

fn stream_send_recv_run() {
    if let Some(s) = STREAM_BENCH.try_get() {
        // Borrow the buffers in-place via LvCell::as_ptr(); avoids the
        // 2× __aeabi_memcpy(128) per iteration that the prior
        // `get()` (Copy) + `set()` (Copy) round-trip emitted on
        // Cortex-M (was +4.8 µs / iteration in the bench report).
        // SAFETY: the bench runner is single-threaded between setup
        // and teardown, satisfying the LvCell single-access invariant;
        // no other thread holds a reference to this cell while we
        // build a temporary &mut T from the raw pointer.
        let bufs_ptr = STREAM_BUFS.get().as_ptr();
        let bufs = unsafe { &mut *bufs_ptr };
        let _ = s.send(&bufs.0);
        let _ = s.recv(&mut bufs.1);
    }
}

fn stream_send_recv_teardown() {
    STREAM_BENCH.shutdown();
}

#[cfg(not(zero_heap))]
fn stream_create_destroy_run() {
    let _s = ove::stream!(STREAM_BUF_SIZE, 1);
}

fn stream_producer() {
    while !STREAM_DONE.load(Ordering::Relaxed) {
        if let Some(s) = STREAM_BENCH.try_get() {
            let bufs = STREAM_BUFS.get().get();
            let _ = s.send(&bufs.0);
        } else {
            // See comment on contention_thread.
            Thread::yield_now();
        }
    }
}

fn stream_throughput_setup() {
    STREAM_DONE.store(false, Ordering::Relaxed);
    STREAM_BUFS
        .get()
        .set(([0xBB; STREAM_MSG_SIZE], [0u8; STREAM_MSG_SIZE]));
    STREAM_BENCH.init(ove::stream!(STREAM_BUF_SIZE, 1));
    STREAM_PRODUCER_TH.init(ove::thread!(
        "strm_prod",
        stream_producer,
        Priority::Normal,
        2048
    ));
}

fn stream_throughput_run() {
    if let Some(s) = STREAM_BENCH.try_get() {
        let cell = STREAM_BUFS.get();
        let mut bufs = cell.get();
        let _ = s.recv(&mut bufs.1);
        cell.set(bufs);
    }
}

fn stream_throughput_teardown() {
    STREAM_DONE.store(true, Ordering::Relaxed);
    if let Some(s) = STREAM_BENCH.try_get() {
        let cell = STREAM_BUFS.get();
        let mut bufs = cell.get();
        let _ = s.try_recv_for(&mut bufs.1, core::time::Duration::from_millis(100));
        cell.set(bufs);
    }
    Thread::sleep_ms(10);
    STREAM_PRODUCER_TH.shutdown();
    STREAM_BENCH.shutdown();
}

#[cfg(not(zero_heap))]
fn stream_memory_run() {
    STREAM_MEM.try_init(ove::stream!(STREAM_BUF_SIZE, 1)).ok();
}
#[cfg(not(zero_heap))]
fn stream_memory_teardown() {
    STREAM_MEM.shutdown();
}

#[cfg(not(zero_heap))]
bench_case!(static STREAM_MEMORY: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: stream_memory_run,
    teardown: stream_memory_teardown,
});
bench_case!(static STREAM_SEND_RECV_64B: BenchCase = {
    name: b"send_recv_64B\0",
    kind: BenchType::Latency,
    run: stream_send_recv_run,
    setup: stream_send_recv_setup,
    teardown: stream_send_recv_teardown,
});
#[cfg(not(zero_heap))]
bench_case!(static STREAM_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: stream_create_destroy_run,
});
bench_case!(static STREAM_THROUGHPUT: BenchCase = {
    name: b"throughput\0",
    kind: BenchType::Throughput,
    run: stream_throughput_run,
    setup: stream_throughput_setup,
    teardown: stream_throughput_teardown,
});

bench_suite!(
    symbol = bench_suite_stream,
    name = b"stream\0",
    enabled = stream_is_enabled,
    cases = [
        #[cfg(not(zero_heap))]
        STREAM_MEMORY,
        STREAM_SEND_RECV_64B,
        #[cfg(not(zero_heap))]
        STREAM_CREATE_DESTROY,
        STREAM_THROUGHPUT
    ],
);

// =========================================================================
//  Suite registry & runner
// =========================================================================

// We read the suite statics by reference so the harness can consume them.
// They're declared via `bench_suite!` with `#[unsafe(no_mangle)]` —
// the C harness also links against them.

fn benchmark_runner() {
    // Use raw byte slices via ove::log to avoid pulling in `core::fmt::Write`
    // from this code path — keeps text size down (~3 KiB of Display/Debug
    // formatters elided) and makes the Rust binary's hot-section flash
    // layout closer to the C binary's, which improves cross-binding
    // benchmark comparability (see C4 caveat in bench report).
    ove::log(b"[I] === oveRTOS Benchmark Suite ===\n");
    ove::log(b"[I] Iterations: 1000  Warmup: 100\n");

    let suites: [&CBenchSuite; 12] = [
        &bench_suite_time,
        &bench_suite_thread,
        &bench_suite_sync,
        &bench_suite_queue,
        &bench_suite_timer,
        &bench_suite_eventgroup,
        &bench_suite_workqueue,
        &bench_suite_stream,
        crate::bench::native_posix_suite(),
        crate::bench::native_freertos_suite(),
        crate::bench::native_nuttx_suite(),
        crate::bench::native_zephyr_suite(),
    ];

    for suite in &suites {
        crate::bench::run_suite(suite);
    }

    ove::log(b"[I] === Benchmark complete ===\n");
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

#[ove::main]
fn app_main() {
    ove::log(b"[I] Benchmark app: init\n");

    // Stream I/O scratch buffers shared between test helpers.
    STREAM_BUFS.init(LvCell::new((
        [0u8; STREAM_MSG_SIZE],
        [0u8; STREAM_MSG_SIZE],
    )));

    let _runner = ove::thread!("bench_run", benchmark_runner, Priority::Normal, 8192);

    // The bench creates+destroys kernel resources during measurement
    // (helper threads in setup, queues/timers in *_create_destroy cases),
    // so we bypass `ove::run()`'s zero-heap auto-lock and start the
    // scheduler directly — matching the C/CPP benches.  Without this,
    // NuttX zero-heap traps `pthread_create`'s kmm_zalloc with ENOMEM
    // and stalls Rust+Zig benches in `ctx_switch_setup`.
    ove::start_scheduler();

    ove::log(b"[I] Benchmark app: shutdown\n");
}

mod bench_cyccnt;
