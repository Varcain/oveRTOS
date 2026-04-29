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
    Thread, Timer, WAIT_FOREVER, Work, Workqueue,
};

// =========================================================================
//  Work handler — safe Rust fn, wrapped into a C trampoline by work_handler!
// =========================================================================

fn wq_work_handler() {
    WQ_WORK_EXECUTED.store(true, core::sync::atomic::Ordering::Relaxed);
    if let Some(sem) = WQ_WORK_SEM.try_get() {
        sem.give();
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

ove::shared!(THREAD_BENCH_TH: Thread);
ove::shared!(THREAD_PING_SEM: Semaphore);
ove::shared!(THREAD_PONG_SEM: Semaphore);
static THREAD_CTX_SWITCH_DONE: AtomicBool = AtomicBool::new(false);

fn thread_is_enabled() -> bool {
    true
}

fn dummy_thread() {}

fn thread_create_destroy_run() {
    let _th = ove::thread!("bench_tmp", dummy_thread, Priority::Low, 1024);
}

fn thread_yield_run() {
    Thread::yield_now();
}

fn thread_sleep_1ms_run() {
    Thread::sleep_ms(1);
}

fn pong_thread() {
    while !THREAD_CTX_SWITCH_DONE.load(Ordering::Relaxed) {
        if let (Some(ping), Some(pong)) = (THREAD_PING_SEM.try_get(), THREAD_PONG_SEM.try_get()) {
            let _ = ping.take(WAIT_FOREVER);
            pong.give();
        } else {
            break;
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
        ping.give();
        let _ = pong.take(WAIT_FOREVER);
    }
}

fn ctx_switch_teardown() {
    THREAD_CTX_SWITCH_DONE.store(true, Ordering::Relaxed);
    if let Some(ping) = THREAD_PING_SEM.try_get() {
        ping.give();
    }
    Thread::sleep_ms(10);
    THREAD_BENCH_TH.shutdown();
    THREAD_PING_SEM.shutdown();
    THREAD_PONG_SEM.shutdown();
}

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
        THREAD_CREATE_DESTROY,
        THREAD_YIELD,
        THREAD_SLEEP_1MS,
        THREAD_CTX_SWITCH
    ],
);

// =========================================================================
//  Suite: sync
// =========================================================================

ove::shared!(SYNC_MTX: Mutex);
ove::shared!(SYNC_SEM: Semaphore);
ove::shared!(SYNC_EVT: Event);
ove::shared!(SYNC_EVT_ACK: Event);
ove::shared!(SYNC_CV: CondVar);
ove::shared!(SYNC_CV_MTX: Mutex);
ove::shared!(SYNC_RMTX: RecursiveMutex);
ove::shared!(SYNC_CONTENTION_TH: Thread);
ove::shared!(SYNC_EVT_TH: Thread);
ove::shared!(SYNC_CV_TH: Thread);
ove::shared!(SYNC_MEM_MUTEX: Mutex);
ove::shared!(SYNC_MEM_SEM: Semaphore);
ove::shared!(SYNC_MEM_EVENT: Event);
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
    SYNC_MTX.init(ove::mutex!());
}
fn mutex_lock_unlock_run() {
    if let Some(m) = SYNC_MTX.try_get() {
        let _ = m.lock(WAIT_FOREVER);
        m.unlock();
    }
}
fn mutex_lock_unlock_teardown() {
    SYNC_MTX.shutdown();
}

// --- Mutex create/destroy ---
fn mutex_create_destroy_run() {
    let _m = ove::mutex!();
}

// --- Mutex contention (2-thread throughput) ---
fn contention_thread() {
    while !SYNC_CONTENTION_DONE.load(Ordering::Relaxed) {
        if let Some(m) = SYNC_MTX.try_get() {
            let _ = m.lock(WAIT_FOREVER);
            SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
            m.unlock();
        } else {
            break;
        }
    }
}

fn mutex_contention_setup() {
    SYNC_CONTENTION_DONE.store(false, Ordering::Relaxed);
    SYNC_CONTENTION_COUNT.store(0, Ordering::Relaxed);
    SYNC_MTX.init(ove::mutex!());
    SYNC_CONTENTION_TH.init(ove::thread!(
        "contention",
        contention_thread,
        Priority::Normal,
        2048
    ));
}

fn mutex_contention_run() {
    if let Some(m) = SYNC_MTX.try_get() {
        let _ = m.lock(WAIT_FOREVER);
        SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
        m.unlock();
    }
}

fn mutex_contention_teardown() {
    SYNC_CONTENTION_DONE.store(true, Ordering::Relaxed);
    Thread::sleep_ms(10);
    SYNC_CONTENTION_TH.shutdown();
    SYNC_MTX.shutdown();
}

// --- Mutex memory ---
fn mutex_memory_run() {
    SYNC_MEM_MUTEX.try_init(ove::mutex!()).ok();
}
fn mutex_memory_teardown() {
    SYNC_MEM_MUTEX.shutdown();
}

// --- Semaphore take/give ---
fn sem_take_give_setup() {
    SYNC_SEM.init(ove::semaphore!(1, 1));
}
fn sem_take_give_run() {
    if let Some(s) = SYNC_SEM.try_get() {
        let _ = s.take(WAIT_FOREVER);
        s.give();
    }
}
fn sem_take_give_teardown() {
    SYNC_SEM.shutdown();
}

// --- Semaphore create/destroy ---
fn sem_create_destroy_run() {
    let _s = ove::semaphore!(0, 1);
}

// --- Semaphore memory ---
fn sem_memory_run() {
    SYNC_MEM_SEM.try_init(ove::semaphore!(0, 1)).ok();
}
fn sem_memory_teardown() {
    SYNC_MEM_SEM.shutdown();
}

// --- Event signal/wait ---
fn evt_signaler() {
    while !SYNC_EVT_DONE.load(Ordering::Relaxed) {
        if let (Some(e), Some(ack)) = (SYNC_EVT.try_get(), SYNC_EVT_ACK.try_get()) {
            e.signal();
            let _ = ack.wait(WAIT_FOREVER);
        } else {
            break;
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
        let _ = e.wait(WAIT_FOREVER);
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

// --- Event memory ---
fn event_memory_run() {
    SYNC_MEM_EVENT.try_init(ove::event!()).ok();
}
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
            break;
        }
        Thread::yield_now();
    }
}

fn condvar_signal_wait_setup() {
    SYNC_CV_DONE.store(false, Ordering::Relaxed);
    SYNC_CV_MTX.init(ove::mutex!());
    SYNC_CV.init(ove::condvar!());
    SYNC_CV_TH.init(ove::thread!("cv_sig", cv_signaler, Priority::Normal, 1024));
}

fn condvar_signal_wait_run() {
    if let (Some(mtx), Some(cv)) = (SYNC_CV_MTX.try_get(), SYNC_CV.try_get()) {
        let _ = mtx.lock(WAIT_FOREVER);
        let _ = cv.wait(mtx, 10);
        mtx.unlock();
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

// --- Condvar memory ---
fn condvar_memory_run() {
    SYNC_MEM_CONDVAR.try_init(ove::condvar!()).ok();
}
fn condvar_memory_teardown() {
    SYNC_MEM_CONDVAR.shutdown();
}

// --- Recursive mutex lock/unlock ---
fn rmtx_lock_unlock_setup() {
    SYNC_RMTX.init(ove::recursive_mutex!());
}
fn rmtx_lock_unlock_run() {
    if let Some(rm) = SYNC_RMTX.try_get() {
        let _ = rm.lock(WAIT_FOREVER);
        rm.unlock();
    }
}
fn rmtx_lock_unlock_teardown() {
    SYNC_RMTX.shutdown();
}

bench_case!(static MUTEX_MEMORY: BenchCase = {
    name: b"mutex_memory\0",
    kind: BenchType::Memory,
    run: mutex_memory_run,
    teardown: mutex_memory_teardown,
});
bench_case!(static SEM_MEMORY: BenchCase = {
    name: b"sem_memory\0",
    kind: BenchType::Memory,
    run: sem_memory_run,
    teardown: sem_memory_teardown,
});
bench_case!(static EVENT_MEMORY: BenchCase = {
    name: b"event_memory\0",
    kind: BenchType::Memory,
    run: event_memory_run,
    teardown: event_memory_teardown,
});
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
        MUTEX_MEMORY,
        SEM_MEMORY,
        EVENT_MEMORY,
        CONDVAR_MEMORY,
        MUTEX_LOCK_UNLOCK,
        MUTEX_CREATE_DESTROY,
        MUTEX_CONTENTION_2T,
        SEM_TAKE_GIVE,
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
ove::shared!(QUEUE_PRODUCER_TH: Thread);
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
        let _ = q.send(&val, WAIT_FOREVER);
        let _ = q.receive(WAIT_FOREVER);
    }
}
fn queue_send_recv_teardown() {
    QUEUE_SEND_RECV_Q.shutdown();
}

fn queue_create_destroy_run() {
    let _q = ove::queue!(u32, 8);
}

fn producer_thread() {
    let mut val: u32 = 0;
    while !QUEUE_THROUGHPUT_DONE.load(Ordering::Relaxed) {
        if let Some(q) = QUEUE_THROUGHPUT_Q.try_get() {
            let _ = q.send(&val, WAIT_FOREVER);
            val = val.wrapping_add(1);
        } else {
            break;
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
        let _ = q.receive(WAIT_FOREVER);
    }
}

fn queue_throughput_teardown() {
    QUEUE_THROUGHPUT_DONE.store(true, Ordering::Relaxed);
    if let Some(q) = QUEUE_THROUGHPUT_Q.try_get() {
        let _ = q.receive(100);
    }
    Thread::sleep_ms(10);
    QUEUE_PRODUCER_TH.shutdown();
    QUEUE_THROUGHPUT_Q.shutdown();
}

fn queue_memory_run() {
    QUEUE_MEM_Q.try_init(ove::queue!(u32, 8)).ok();
}
fn queue_memory_teardown() {
    QUEUE_MEM_Q.shutdown();
}

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
        QUEUE_MEMORY,
        QUEUE_SEND_RECEIVE,
        QUEUE_CREATE_DESTROY,
        QUEUE_THROUGHPUT_2T
    ],
);

// =========================================================================
//  Suite: timer
// =========================================================================

ove::shared!(TIMER_TMR: Timer);
ove::shared!(TIMER_MEM_TMR: Timer);

fn timer_is_enabled() -> bool {
    true
}

fn timer_dummy_cb() {}

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

fn timer_memory_run() {
    TIMER_MEM_TMR
        .try_init(ove::timer!(timer_dummy_cb, 1000, false))
        .ok();
}
fn timer_memory_teardown() {
    TIMER_MEM_TMR.shutdown();
}

bench_case!(static TIMER_MEMORY: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: timer_memory_run,
    teardown: timer_memory_teardown,
});
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
    cases = [TIMER_MEMORY, TIMER_CREATE_DESTROY, TIMER_START_STOP],
);

// =========================================================================
//  Suite: eventgroup
// =========================================================================

ove::shared!(EG_BENCH: EventGroup);
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

fn eg_create_destroy_run() {
    let _eg = ove::eventgroup!();
}

fn eg_memory_run() {
    EG_MEM.try_init(ove::eventgroup!()).ok();
}
fn eg_memory_teardown() {
    EG_MEM.shutdown();
}

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
bench_case!(static EG_CREATE_DESTROY: BenchCase = {
    name: b"create_destroy\0",
    kind: BenchType::Latency,
    run: eg_create_destroy_run,
});

bench_suite!(
    symbol = bench_suite_eventgroup,
    name = b"eventgroup\0",
    enabled = eventgroup_is_enabled,
    cases = [EG_MEMORY, EG_SET_GET, EG_CREATE_DESTROY],
);

// =========================================================================
//  Suite: workqueue
// =========================================================================

ove::shared!(WQ_BENCH: Workqueue);
ove::shared!(WQ_WORK: Work);
ove::shared!(WQ_WORK_SEM: Semaphore);
ove::shared!(WQ_MEM: Workqueue);
static WQ_WORK_EXECUTED: AtomicBool = AtomicBool::new(false);

fn workqueue_is_enabled() -> bool {
    true
}

fn wq_create_destroy_run() {
    let _wq = ove::workqueue!("bench_wq", Priority::Normal, 2048);
}

fn wq_submit_setup() {
    WQ_WORK_SEM.init(ove::semaphore!(0, 1));
    WQ_BENCH.init(ove::workqueue!("bench_wq", Priority::Normal, 2048));
    WQ_WORK.init(ove::work!(ove::work_handler!(wq_work_handler)));
}

fn wq_submit_run() {
    WQ_WORK_EXECUTED.store(false, Ordering::Relaxed);
    if let (Some(w), Some(wq), Some(sem)) =
        (WQ_WORK.try_get(), WQ_BENCH.try_get(), WQ_WORK_SEM.try_get())
    {
        let _ = w.submit(wq);
        let _ = sem.take(1000);
    }
}

fn wq_submit_teardown() {
    WQ_WORK.shutdown();
    WQ_BENCH.shutdown();
    WQ_WORK_SEM.shutdown();
}

fn wq_memory_run() {
    WQ_MEM
        .try_init(ove::workqueue!("bench_wq", Priority::Normal, 2048))
        .ok();
}
fn wq_memory_teardown() {
    WQ_MEM.shutdown();
}

bench_case!(static WQ_MEMORY_C: BenchCase = {
    name: b"memory\0",
    kind: BenchType::Memory,
    run: wq_memory_run,
    teardown: wq_memory_teardown,
});
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
    cases = [WQ_MEMORY_C, WQ_CREATE_DESTROY, WQ_SUBMIT_EXECUTE],
);

// =========================================================================
//  Suite: stream
// =========================================================================

const STREAM_BUF_SIZE: usize = 256;
const STREAM_MSG_SIZE: usize = 64;

ove::shared!(STREAM_BENCH: Stream<STREAM_BUF_SIZE>);
ove::shared!(STREAM_PRODUCER_TH: Thread);
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
        let _ = s.send(&bufs.0, WAIT_FOREVER);
        let _ = s.receive(&mut bufs.1, WAIT_FOREVER);
    }
}

fn stream_send_recv_teardown() {
    STREAM_BENCH.shutdown();
}

fn stream_create_destroy_run() {
    let _s = ove::stream!(STREAM_BUF_SIZE, 1);
}

fn stream_producer() {
    while !STREAM_DONE.load(Ordering::Relaxed) {
        if let Some(s) = STREAM_BENCH.try_get() {
            let bufs = STREAM_BUFS.get().get();
            let _ = s.send(&bufs.0, WAIT_FOREVER);
        } else {
            break;
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
        let _ = s.receive(&mut bufs.1, WAIT_FOREVER);
        cell.set(bufs);
    }
}

fn stream_throughput_teardown() {
    STREAM_DONE.store(true, Ordering::Relaxed);
    if let Some(s) = STREAM_BENCH.try_get() {
        let cell = STREAM_BUFS.get();
        let mut bufs = cell.get();
        let _ = s.receive(&mut bufs.1, 100);
        cell.set(bufs);
    }
    Thread::sleep_ms(10);
    STREAM_PRODUCER_TH.shutdown();
    STREAM_BENCH.shutdown();
}

fn stream_memory_run() {
    STREAM_MEM.try_init(ove::stream!(STREAM_BUF_SIZE, 1)).ok();
}
fn stream_memory_teardown() {
    STREAM_MEM.shutdown();
}

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
        STREAM_MEMORY,
        STREAM_SEND_RECV_64B,
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

    let suites: [&CBenchSuite; 11] = [
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
    ];

    for suite in &suites {
        crate::bench::run_suite(suite);
    }

    ove::log(b"[I] === Benchmark complete ===\n");
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log(b"[I] Benchmark app: init\n");

    // Stream I/O scratch buffers shared between test helpers.
    STREAM_BUFS.init(LvCell::new((
        [0u8; STREAM_MSG_SIZE],
        [0u8; STREAM_MSG_SIZE],
    )));

    let _runner = ove::thread!("bench_run", benchmark_runner, Priority::Normal, 8192);

    ove::run();

    ove::log(b"[I] Benchmark app: shutdown\n");
}

ove::main!(app_main);
