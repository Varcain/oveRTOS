// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Benchmark Application (Rust)
//!
//! Measures latency, throughput, and memory usage of all RTOS abstractions
//! using safe Rust bindings. Suite symbols are exported as `#[no_mangle]`
//! C-compatible statics so the shared C harness (`bench_run_case`,
//! `bench_print_*`) can drive them.
//!
//! All resource creation uses the unified `ove::*!` macros (e.g.
//! `ove::thread!`, `ove::timer!`) so the benchmark builds and runs in
//! both heap and zero-heap configurations.

#![cfg_attr(not(feature = "std"), no_std)]

use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use ove::ffi;
use ove::time;
use ove::{
    CondVar, Event, EventGroup, Mutex, Priority, Queue, RecursiveMutex, Semaphore, Stream, Thread,
    Timer, Work, Workqueue, WAIT_FOREVER,
};

// ---------------------------------------------------------------------------
// C-compatible types matching benchmark.h
// ---------------------------------------------------------------------------

/// Benchmark type enum, matching `bench_type_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub enum BenchType {
    Latency = 0,
    Throughput = 1,
    Memory = 2,
}

/// Benchmark case descriptor, matching `bench_case_t`.
#[repr(C)]
pub struct BenchCase {
    pub name: *const core::ffi::c_char,
    pub bench_type: BenchType,
    pub setup: Option<unsafe extern "C" fn(*mut c_void)>,
    pub run: Option<unsafe extern "C" fn(*mut c_void)>,
    pub teardown: Option<unsafe extern "C" fn(*mut c_void)>,
    pub iterations: u32,
}

// SAFETY: BenchCase contains only raw pointers and plain data. The pointed-to
// data (name strings, function pointers) has `'static` lifetime because they
// come from const statics.
unsafe impl Sync for BenchCase {}

/// Benchmark result, matching `bench_result_t`.
#[repr(C)]
pub struct BenchResult {
    pub min_ns: u64,
    pub max_ns: u64,
    pub total_ns: u64,
    pub count: u32,
    pub ops_per_sec: u32,
    pub heap_delta: i32,
}

/// Benchmark suite descriptor, matching `bench_suite_t`.
#[repr(C)]
pub struct BenchSuite {
    pub name: *const core::ffi::c_char,
    pub is_enabled: Option<unsafe extern "C" fn() -> i32>,
    pub cases: *const BenchCase,
    pub case_count: u32,
}

// SAFETY: BenchSuite contains only raw pointers and plain data pointing to
// `'static` const data.
unsafe impl Sync for BenchSuite {}

// ---------------------------------------------------------------------------
// C harness imports (linked from c_sources in app.yaml)
// ---------------------------------------------------------------------------

unsafe extern "C" {
    fn bench_run_case(bc: *const BenchCase, result: *mut BenchResult);
    fn bench_print_header(suite_name: *const core::ffi::c_char);
    fn bench_print_result(bc: *const BenchCase, result: *const BenchResult);
    fn bench_print_footer();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Null pointer constant for optional function pointers.
const NONE: Option<unsafe extern "C" fn(*mut c_void)> = None;

// ===========================================================================
// Suite: time
// ===========================================================================

unsafe extern "C" fn time_get_us_overhead_run(_ctx: *mut c_void) {
    let _ = time::get_us();
}

unsafe extern "C" fn delay_1ms_run(_ctx: *mut c_void) {
    time::delay_ms(1);
}

unsafe extern "C" fn time_is_enabled() -> i32 {
    1
}

static TIME_CASES: [BenchCase; 2] = [
    BenchCase {
        name: b"time_get_us_overhead\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(time_get_us_overhead_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"delay_1ms\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(delay_1ms_run),
        teardown: NONE,
        iterations: 100,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_time: BenchSuite = BenchSuite {
    name: b"time\0".as_ptr() as *const _,
    is_enabled: Some(time_is_enabled),
    cases: TIME_CASES.as_ptr(),
    case_count: TIME_CASES.len() as u32,
};

// ===========================================================================
// Suite: thread
// ===========================================================================

static mut THREAD_BENCH_TH: Option<Thread> = None;
static mut THREAD_PING_SEM: Option<Semaphore> = None;
static mut THREAD_PONG_SEM: Option<Semaphore> = None;
static THREAD_CTX_SWITCH_DONE: AtomicBool = AtomicBool::new(false);

fn dummy_thread() {}

unsafe extern "C" fn thread_create_destroy_run(_ctx: *mut c_void) {
    let _th = ove::thread!("bench_tmp", dummy_thread, Priority::Low, 1024);
}

unsafe extern "C" fn thread_yield_run(_ctx: *mut c_void) {
    Thread::yield_now();
}

unsafe extern "C" fn thread_sleep_1ms_run(_ctx: *mut c_void) {
    Thread::sleep_ms(1);
}

fn pong_thread() {
    while !THREAD_CTX_SWITCH_DONE.load(Ordering::Relaxed) {
        unsafe {
            let _ = (*(&raw const THREAD_PING_SEM)).as_ref().unwrap().take(WAIT_FOREVER);
            (*(&raw const THREAD_PONG_SEM)).as_ref().unwrap().give();
        }
    }
}

unsafe extern "C" fn ctx_switch_setup(_ctx: *mut c_void) {
    THREAD_CTX_SWITCH_DONE.store(false, Ordering::Relaxed);
    unsafe {
        *(&raw mut THREAD_PING_SEM) = Some(ove::semaphore!(0, 1));
        *(&raw mut THREAD_PONG_SEM) = Some(ove::semaphore!(0, 1));
        *(&raw mut THREAD_BENCH_TH) = Some(
            ove::thread!("pong", pong_thread, Priority::Normal, 2048),
        );
    }
}

unsafe extern "C" fn ctx_switch_run(_ctx: *mut c_void) {
    unsafe {
        (*(&raw const THREAD_PING_SEM)).as_ref().unwrap().give();
        let _ = (*(&raw const THREAD_PONG_SEM)).as_ref().unwrap().take(WAIT_FOREVER);
    }
}

unsafe extern "C" fn ctx_switch_teardown(_ctx: *mut c_void) {
    THREAD_CTX_SWITCH_DONE.store(true, Ordering::Relaxed);
    unsafe {
        (*(&raw const THREAD_PING_SEM)).as_ref().unwrap().give();
        Thread::sleep_ms(10);
        *(&raw mut THREAD_BENCH_TH) = None;
        *(&raw mut THREAD_PING_SEM) = None;
        *(&raw mut THREAD_PONG_SEM) = None;
    }
}

unsafe extern "C" fn thread_is_enabled() -> i32 {
    1
}

static THREAD_CASES: [BenchCase; 4] = [
    BenchCase {
        name: b"create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(thread_create_destroy_run),
        teardown: NONE,
        iterations: 200,
    },
    BenchCase {
        name: b"yield\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(thread_yield_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"sleep_1ms\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(thread_sleep_1ms_run),
        teardown: NONE,
        iterations: 100,
    },
    BenchCase {
        name: b"context_switch\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(ctx_switch_setup),
        run: Some(ctx_switch_run),
        teardown: Some(ctx_switch_teardown),
        iterations: 500,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_thread: BenchSuite = BenchSuite {
    name: b"thread\0".as_ptr() as *const _,
    is_enabled: Some(thread_is_enabled),
    cases: THREAD_CASES.as_ptr(),
    case_count: THREAD_CASES.len() as u32,
};

// ===========================================================================
// Suite: sync
// ===========================================================================

static mut SYNC_MTX: Option<Mutex> = None;
static mut SYNC_SEM: Option<Semaphore> = None;
static mut SYNC_EVT: Option<Event> = None;
static mut SYNC_CV: Option<CondVar> = None;
static mut SYNC_CV_MTX: Option<Mutex> = None;
static mut SYNC_RMTX: Option<RecursiveMutex> = None;
static mut SYNC_CONTENTION_TH: Option<Thread> = None;
static SYNC_CONTENTION_DONE: AtomicBool = AtomicBool::new(false);
static SYNC_CONTENTION_COUNT: AtomicU32 = AtomicU32::new(0);
static mut SYNC_EVT_TH: Option<Thread> = None;
static SYNC_EVT_DONE: AtomicBool = AtomicBool::new(false);
static mut SYNC_CV_TH: Option<Thread> = None;
static SYNC_CV_DONE: AtomicBool = AtomicBool::new(false);

// --- Mutex lock/unlock ---

unsafe extern "C" fn mutex_lock_unlock_setup(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MTX) = Some(ove::mutex!()) };
}

unsafe extern "C" fn mutex_lock_unlock_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const SYNC_MTX)).as_ref().unwrap().lock(WAIT_FOREVER);
        (*(&raw const SYNC_MTX)).as_ref().unwrap().unlock();
    }
}

unsafe extern "C" fn mutex_lock_unlock_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MTX) = None };
}

// --- Mutex create/destroy ---

unsafe extern "C" fn mutex_create_destroy_run(_ctx: *mut c_void) {
    let _m = ove::mutex!();
}

// --- Mutex contention (2-thread throughput) ---

fn contention_thread() {
    while !SYNC_CONTENTION_DONE.load(Ordering::Relaxed) {
        unsafe {
            let _ = (*(&raw const SYNC_MTX)).as_ref().unwrap().lock(WAIT_FOREVER);
        }
        SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
        unsafe {
            (*(&raw const SYNC_MTX)).as_ref().unwrap().unlock();
        }
    }
}

unsafe extern "C" fn mutex_contention_setup(_ctx: *mut c_void) {
    SYNC_CONTENTION_DONE.store(false, Ordering::Relaxed);
    SYNC_CONTENTION_COUNT.store(0, Ordering::Relaxed);
    unsafe {
        *(&raw mut SYNC_MTX) = Some(ove::mutex!());
        *(&raw mut SYNC_CONTENTION_TH) = Some(
            ove::thread!("contention", contention_thread, Priority::Normal, 2048),
        );
    }
}

unsafe extern "C" fn mutex_contention_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const SYNC_MTX)).as_ref().unwrap().lock(WAIT_FOREVER);
    }
    SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
    unsafe {
        (*(&raw const SYNC_MTX)).as_ref().unwrap().unlock();
    }
}

unsafe extern "C" fn mutex_contention_teardown(_ctx: *mut c_void) {
    SYNC_CONTENTION_DONE.store(true, Ordering::Relaxed);
    unsafe {
        Thread::sleep_ms(10);
        *(&raw mut SYNC_CONTENTION_TH) = None;
        *(&raw mut SYNC_MTX) = None;
    }
}

// --- Mutex memory ---

static mut SYNC_MEM_MUTEX: Option<Mutex> = None;

unsafe extern "C" fn mutex_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_MUTEX) = Some(ove::mutex!()) };
}

unsafe extern "C" fn mutex_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_MUTEX) = None };
}

// --- Semaphore take/give ---

unsafe extern "C" fn sem_take_give_setup(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_SEM) = Some(ove::semaphore!(1, 1)) };
}

unsafe extern "C" fn sem_take_give_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const SYNC_SEM)).as_ref().unwrap().take(WAIT_FOREVER);
        (*(&raw const SYNC_SEM)).as_ref().unwrap().give();
    }
}

unsafe extern "C" fn sem_take_give_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_SEM) = None };
}

// --- Semaphore create/destroy ---

unsafe extern "C" fn sem_create_destroy_run(_ctx: *mut c_void) {
    let _s = ove::semaphore!(0, 1);
}

// --- Semaphore memory ---

static mut SYNC_MEM_SEM: Option<Semaphore> = None;

unsafe extern "C" fn sem_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_SEM) = Some(ove::semaphore!(0, 1)) };
}

unsafe extern "C" fn sem_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_SEM) = None };
}

// --- Event signal/wait ---

fn evt_signaler() {
    while !SYNC_EVT_DONE.load(Ordering::Relaxed) {
        unsafe {
            (*(&raw const SYNC_EVT)).as_ref().unwrap().signal();
        }
        Thread::yield_now();
    }
}

unsafe extern "C" fn event_signal_wait_setup(_ctx: *mut c_void) {
    SYNC_EVT_DONE.store(false, Ordering::Relaxed);
    unsafe {
        *(&raw mut SYNC_EVT) = Some(ove::event!());
        *(&raw mut SYNC_EVT_TH) = Some(
            ove::thread!("evt_sig", evt_signaler, Priority::Normal, 1024),
        );
    }
}

unsafe extern "C" fn event_signal_wait_run(_ctx: *mut c_void) {
    unsafe { let _ = (*(&raw const SYNC_EVT)).as_ref().unwrap().wait(10); }
}

unsafe extern "C" fn event_signal_wait_teardown(_ctx: *mut c_void) {
    SYNC_EVT_DONE.store(true, Ordering::Relaxed);
    unsafe {
        Thread::sleep_ms(10);
        *(&raw mut SYNC_EVT_TH) = None;
        *(&raw mut SYNC_EVT) = None;
    }
}

// --- Event memory ---

static mut SYNC_MEM_EVENT: Option<Event> = None;

unsafe extern "C" fn event_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_EVENT) = Some(ove::event!()) };
}

unsafe extern "C" fn event_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_EVENT) = None };
}

// --- Condvar signal/wait ---

fn cv_signaler() {
    while !SYNC_CV_DONE.load(Ordering::Relaxed) {
        unsafe {
            (*(&raw const SYNC_CV)).as_ref().unwrap().signal();
        }
        Thread::yield_now();
    }
}

unsafe extern "C" fn condvar_signal_wait_setup(_ctx: *mut c_void) {
    SYNC_CV_DONE.store(false, Ordering::Relaxed);
    unsafe {
        *(&raw mut SYNC_CV_MTX) = Some(ove::mutex!());
        *(&raw mut SYNC_CV) = Some(ove::condvar!());
        *(&raw mut SYNC_CV_TH) = Some(
            ove::thread!("cv_sig", cv_signaler, Priority::Normal, 1024),
        );
    }
}

unsafe extern "C" fn condvar_signal_wait_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const SYNC_CV_MTX)).as_ref().unwrap().lock(WAIT_FOREVER);
        let _ = (*(&raw const SYNC_CV)).as_ref().unwrap().wait((*(&raw const SYNC_CV_MTX)).as_ref().unwrap(), 10);
        (*(&raw const SYNC_CV_MTX)).as_ref().unwrap().unlock();
    }
}

unsafe extern "C" fn condvar_signal_wait_teardown(_ctx: *mut c_void) {
    SYNC_CV_DONE.store(true, Ordering::Relaxed);
    unsafe {
        (*(&raw const SYNC_CV)).as_ref().unwrap().signal();
        Thread::sleep_ms(10);
        *(&raw mut SYNC_CV_TH) = None;
        *(&raw mut SYNC_CV) = None;
        *(&raw mut SYNC_CV_MTX) = None;
    }
}

// --- Condvar memory ---

static mut SYNC_MEM_CONDVAR: Option<CondVar> = None;

unsafe extern "C" fn condvar_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_CONDVAR) = Some(ove::condvar!()) };
}

unsafe extern "C" fn condvar_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_MEM_CONDVAR) = None };
}

// --- Recursive mutex lock/unlock ---

unsafe extern "C" fn rmtx_lock_unlock_setup(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_RMTX) = Some(ove::recursive_mutex!()) };
}

unsafe extern "C" fn rmtx_lock_unlock_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const SYNC_RMTX)).as_ref().unwrap().lock(WAIT_FOREVER);
        (*(&raw const SYNC_RMTX)).as_ref().unwrap().unlock();
    }
}

unsafe extern "C" fn rmtx_lock_unlock_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut SYNC_RMTX) = None };
}

// --- sync suite ---

unsafe extern "C" fn sync_is_enabled() -> i32 {
    1
}

static SYNC_CASES: [BenchCase; 12] = [
    // Memory tests first -- before thread-heavy tests affect heap state
    BenchCase {
        name: b"mutex_memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(mutex_memory_run),
        teardown: Some(mutex_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"sem_memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(sem_memory_run),
        teardown: Some(sem_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"event_memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(event_memory_run),
        teardown: Some(event_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"condvar_memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(condvar_memory_run),
        teardown: Some(condvar_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"mutex_lock_unlock\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(mutex_lock_unlock_setup),
        run: Some(mutex_lock_unlock_run),
        teardown: Some(mutex_lock_unlock_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"mutex_create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(mutex_create_destroy_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"mutex_contention_2t\0".as_ptr() as *const _,
        bench_type: BenchType::Throughput,
        setup: Some(mutex_contention_setup),
        run: Some(mutex_contention_run),
        teardown: Some(mutex_contention_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"sem_take_give\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(sem_take_give_setup),
        run: Some(sem_take_give_run),
        teardown: Some(sem_take_give_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"sem_create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(sem_create_destroy_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"event_signal_wait\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(event_signal_wait_setup),
        run: Some(event_signal_wait_run),
        teardown: Some(event_signal_wait_teardown),
        iterations: 500,
    },
    BenchCase {
        name: b"condvar_signal_wait\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(condvar_signal_wait_setup),
        run: Some(condvar_signal_wait_run),
        teardown: Some(condvar_signal_wait_teardown),
        iterations: 500,
    },
    BenchCase {
        name: b"recursive_mutex_lock_unlock\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(rmtx_lock_unlock_setup),
        run: Some(rmtx_lock_unlock_run),
        teardown: Some(rmtx_lock_unlock_teardown),
        iterations: 0,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_sync: BenchSuite = BenchSuite {
    name: b"sync\0".as_ptr() as *const _,
    is_enabled: Some(sync_is_enabled),
    cases: SYNC_CASES.as_ptr(),
    case_count: SYNC_CASES.len() as u32,
};

// ===========================================================================
// Suite: queue
// ===========================================================================

static mut QUEUE_SEND_RECV_Q: Option<Queue<u32, 16>> = None;
static mut QUEUE_THROUGHPUT_Q: Option<Queue<u32, 64>> = None;
static mut QUEUE_PRODUCER_TH: Option<Thread> = None;
static QUEUE_THROUGHPUT_DONE: AtomicBool = AtomicBool::new(false);

// --- send/receive latency ---

unsafe extern "C" fn queue_send_recv_setup(_ctx: *mut c_void) {
    unsafe { *(&raw mut QUEUE_SEND_RECV_Q) = Some(ove::queue!(u32, 16)) };
}

unsafe extern "C" fn queue_send_recv_run(_ctx: *mut c_void) {
    let val: u32 = 42;
    unsafe {
        let _ = (*(&raw const QUEUE_SEND_RECV_Q)).as_ref().unwrap().send(&val, WAIT_FOREVER);
        let _ = (*(&raw const QUEUE_SEND_RECV_Q)).as_ref().unwrap().receive(WAIT_FOREVER);
    }
}

unsafe extern "C" fn queue_send_recv_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut QUEUE_SEND_RECV_Q) = None };
}

// --- create/destroy ---

unsafe extern "C" fn queue_create_destroy_run(_ctx: *mut c_void) {
    let _q = ove::queue!(u32, 8);
}

// --- 2-thread throughput ---

fn producer_thread() {
    let mut val: u32 = 0;
    while !QUEUE_THROUGHPUT_DONE.load(Ordering::Relaxed) {
        unsafe {
            let _ = (*(&raw const QUEUE_THROUGHPUT_Q)).as_ref().unwrap().send(&val, WAIT_FOREVER);
        }
        val = val.wrapping_add(1);
    }
}

unsafe extern "C" fn queue_throughput_setup(_ctx: *mut c_void) {
    QUEUE_THROUGHPUT_DONE.store(false, Ordering::Relaxed);
    unsafe {
        *(&raw mut QUEUE_THROUGHPUT_Q) = Some(ove::queue!(u32, 64));
        *(&raw mut QUEUE_PRODUCER_TH) = Some(
            ove::thread!("q_prod", producer_thread, Priority::Normal, 2048),
        );
    }
}

unsafe extern "C" fn queue_throughput_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const QUEUE_THROUGHPUT_Q)).as_ref().unwrap().receive(WAIT_FOREVER);
    }
}

unsafe extern "C" fn queue_throughput_teardown(_ctx: *mut c_void) {
    QUEUE_THROUGHPUT_DONE.store(true, Ordering::Relaxed);
    // Drain queue so producer unblocks
    unsafe {
        let _ = (*(&raw const QUEUE_THROUGHPUT_Q)).as_ref().unwrap().receive(100);
        Thread::sleep_ms(10);
        *(&raw mut QUEUE_PRODUCER_TH) = None;
        *(&raw mut QUEUE_THROUGHPUT_Q) = None;
    }
}

// --- memory ---

static mut QUEUE_MEM_Q: Option<Queue<u32, 8>> = None;

unsafe extern "C" fn queue_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut QUEUE_MEM_Q) = Some(ove::queue!(u32, 8)) };
}

unsafe extern "C" fn queue_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut QUEUE_MEM_Q) = None };
}

// --- queue suite ---

unsafe extern "C" fn queue_is_enabled() -> i32 {
    1
}

static QUEUE_CASES: [BenchCase; 4] = [
    BenchCase {
        name: b"memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(queue_memory_run),
        teardown: Some(queue_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"send_receive\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(queue_send_recv_setup),
        run: Some(queue_send_recv_run),
        teardown: Some(queue_send_recv_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(queue_create_destroy_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"throughput_2t\0".as_ptr() as *const _,
        bench_type: BenchType::Throughput,
        setup: Some(queue_throughput_setup),
        run: Some(queue_throughput_run),
        teardown: Some(queue_throughput_teardown),
        iterations: 0,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_queue: BenchSuite = BenchSuite {
    name: b"queue\0".as_ptr() as *const _,
    is_enabled: Some(queue_is_enabled),
    cases: QUEUE_CASES.as_ptr(),
    case_count: QUEUE_CASES.len() as u32,
};

// ===========================================================================
// Suite: timer
// ===========================================================================

static mut TIMER_TMR: Option<Timer> = None;

fn timer_dummy_cb() {}

// --- create/destroy ---

unsafe extern "C" fn timer_create_destroy_run(_ctx: *mut c_void) {
    let _t = ove::timer!(timer_dummy_cb, 1000, false);
}

// --- start/stop ---

unsafe extern "C" fn timer_start_stop_setup(_ctx: *mut c_void) {
    unsafe { *(&raw mut TIMER_TMR) = Some(ove::timer!(timer_dummy_cb, 1000, false)) };
}

unsafe extern "C" fn timer_start_stop_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const TIMER_TMR)).as_ref().unwrap().start();
        let _ = (*(&raw const TIMER_TMR)).as_ref().unwrap().stop();
    }
}

unsafe extern "C" fn timer_start_stop_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut TIMER_TMR) = None };
}

// --- memory ---

static mut TIMER_MEM_TMR: Option<Timer> = None;

unsafe extern "C" fn timer_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut TIMER_MEM_TMR) = Some(ove::timer!(timer_dummy_cb, 1000, false)) };
}

unsafe extern "C" fn timer_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut TIMER_MEM_TMR) = None };
}

// --- timer suite ---

unsafe extern "C" fn timer_is_enabled() -> i32 {
    1
}

static TIMER_CASES: [BenchCase; 3] = [
    BenchCase {
        name: b"memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(timer_memory_run),
        teardown: Some(timer_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(timer_create_destroy_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"start_stop\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(timer_start_stop_setup),
        run: Some(timer_start_stop_run),
        teardown: Some(timer_start_stop_teardown),
        iterations: 0,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_timer: BenchSuite = BenchSuite {
    name: b"timer\0".as_ptr() as *const _,
    is_enabled: Some(timer_is_enabled),
    cases: TIMER_CASES.as_ptr(),
    case_count: TIMER_CASES.len() as u32,
};

// ===========================================================================
// Suite: eventgroup
// ===========================================================================

static mut EG_BENCH: Option<EventGroup> = None;

// --- set/get bits ---

unsafe extern "C" fn eg_set_get_setup(_ctx: *mut c_void) {
    unsafe { *(&raw mut EG_BENCH) = Some(ove::eventgroup!()) };
}

unsafe extern "C" fn eg_set_get_run(_ctx: *mut c_void) {
    unsafe {
        (*(&raw const EG_BENCH)).as_ref().unwrap().set_bits(0x01);
        (*(&raw const EG_BENCH)).as_ref().unwrap().get_bits();
        (*(&raw const EG_BENCH)).as_ref().unwrap().clear_bits(0x01);
    }
}

unsafe extern "C" fn eg_set_get_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut EG_BENCH) = None };
}

// --- create/destroy ---

unsafe extern "C" fn eg_create_destroy_run(_ctx: *mut c_void) {
    let _eg = ove::eventgroup!();
}

// --- memory ---

static mut EG_MEM: Option<EventGroup> = None;

unsafe extern "C" fn eg_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut EG_MEM) = Some(ove::eventgroup!()) };
}

unsafe extern "C" fn eg_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut EG_MEM) = None };
}

// --- eventgroup suite ---

unsafe extern "C" fn eventgroup_is_enabled() -> i32 {
    1
}

static EVENTGROUP_CASES: [BenchCase; 3] = [
    BenchCase {
        name: b"memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(eg_memory_run),
        teardown: Some(eg_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"set_get_bits\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(eg_set_get_setup),
        run: Some(eg_set_get_run),
        teardown: Some(eg_set_get_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(eg_create_destroy_run),
        teardown: NONE,
        iterations: 0,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_eventgroup: BenchSuite = BenchSuite {
    name: b"eventgroup\0".as_ptr() as *const _,
    is_enabled: Some(eventgroup_is_enabled),
    cases: EVENTGROUP_CASES.as_ptr(),
    case_count: EVENTGROUP_CASES.len() as u32,
};

// ===========================================================================
// Suite: workqueue
// ===========================================================================

static mut WQ_BENCH: Option<Workqueue> = None;
static mut WQ_WORK: Option<Work> = None;
static WQ_WORK_EXECUTED: AtomicBool = AtomicBool::new(false);
static mut WQ_WORK_SEM: Option<Semaphore> = None;

unsafe extern "C" fn work_handler(_work: ffi::ove_work_t) {
    WQ_WORK_EXECUTED.store(true, Ordering::Relaxed);
    unsafe { (*(&raw const WQ_WORK_SEM)).as_ref().unwrap().give() };
}

// --- create/destroy ---

unsafe extern "C" fn wq_create_destroy_run(_ctx: *mut c_void) {
    let _wq = ove::workqueue!("bench_wq", Priority::Normal, 2048);
}

// --- submit/execute ---

unsafe extern "C" fn wq_submit_setup(_ctx: *mut c_void) {
    unsafe {
        *(&raw mut WQ_WORK_SEM) = Some(ove::semaphore!(0, 1));
        *(&raw mut WQ_BENCH) = Some(ove::workqueue!("bench_wq", Priority::Normal, 2048));
        *(&raw mut WQ_WORK) = Some(ove::work!(Some(work_handler)));
    }
}

unsafe extern "C" fn wq_submit_run(_ctx: *mut c_void) {
    WQ_WORK_EXECUTED.store(false, Ordering::Relaxed);
    unsafe {
        let _ = (*(&raw const WQ_WORK)).as_ref().unwrap().submit((*(&raw const WQ_BENCH)).as_ref().unwrap());
        let _ = (*(&raw const WQ_WORK_SEM)).as_ref().unwrap().take(1000);
    }
}

unsafe extern "C" fn wq_submit_teardown(_ctx: *mut c_void) {
    unsafe {
        *(&raw mut WQ_WORK) = None;
        *(&raw mut WQ_BENCH) = None;
        *(&raw mut WQ_WORK_SEM) = None;
    }
}

// --- memory ---

static mut WQ_MEM: Option<Workqueue> = None;

unsafe extern "C" fn wq_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut WQ_MEM) = Some(ove::workqueue!("bench_wq", Priority::Normal, 2048)) };
}

unsafe extern "C" fn wq_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut WQ_MEM) = None };
}

// --- workqueue suite ---

unsafe extern "C" fn workqueue_is_enabled() -> i32 {
    1
}

static WORKQUEUE_CASES: [BenchCase; 3] = [
    BenchCase {
        name: b"memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(wq_memory_run),
        teardown: Some(wq_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(wq_create_destroy_run),
        teardown: NONE,
        iterations: 200,
    },
    BenchCase {
        name: b"submit_execute\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(wq_submit_setup),
        run: Some(wq_submit_run),
        teardown: Some(wq_submit_teardown),
        iterations: 500,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_workqueue: BenchSuite = BenchSuite {
    name: b"workqueue\0".as_ptr() as *const _,
    is_enabled: Some(workqueue_is_enabled),
    cases: WORKQUEUE_CASES.as_ptr(),
    case_count: WORKQUEUE_CASES.len() as u32,
};

// ===========================================================================
// Suite: stream
// ===========================================================================

const STREAM_BUF_SIZE: usize = 256;
const STREAM_MSG_SIZE: usize = 64;

static mut STREAM_BENCH: Option<Stream<STREAM_BUF_SIZE>> = None;
static mut STREAM_PRODUCER_TH: Option<Thread> = None;
static STREAM_DONE: AtomicBool = AtomicBool::new(false);

static mut STREAM_TX_BUF: [u8; STREAM_MSG_SIZE] = [0u8; STREAM_MSG_SIZE];
static mut STREAM_RX_BUF: [u8; STREAM_MSG_SIZE] = [0u8; STREAM_MSG_SIZE];

// --- send/receive 64B ---

unsafe extern "C" fn stream_send_recv_setup(_ctx: *mut c_void) {
    unsafe {
        *(&raw mut STREAM_BENCH) = Some(ove::stream!(STREAM_BUF_SIZE, 1));
        *(&raw mut STREAM_TX_BUF) = [0xAA; STREAM_MSG_SIZE];
    }
}

unsafe extern "C" fn stream_send_recv_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const STREAM_BENCH)).as_ref().unwrap().send(&*(&raw const STREAM_TX_BUF), WAIT_FOREVER);
        let _ = (*(&raw const STREAM_BENCH)).as_ref().unwrap().receive(&mut *(&raw mut STREAM_RX_BUF), WAIT_FOREVER);
    }
}

unsafe extern "C" fn stream_send_recv_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut STREAM_BENCH) = None };
}

// --- create/destroy ---

unsafe extern "C" fn stream_create_destroy_run(_ctx: *mut c_void) {
    let _s = ove::stream!(STREAM_BUF_SIZE, 1);
}

// --- throughput ---

fn stream_producer() {
    while !STREAM_DONE.load(Ordering::Relaxed) {
        unsafe {
            let _ = (*(&raw const STREAM_BENCH)).as_ref().unwrap().send(&*(&raw const STREAM_TX_BUF), WAIT_FOREVER);
        }
    }
}

unsafe extern "C" fn stream_throughput_setup(_ctx: *mut c_void) {
    STREAM_DONE.store(false, Ordering::Relaxed);
    unsafe {
        *(&raw mut STREAM_TX_BUF) = [0xBB; STREAM_MSG_SIZE];
        *(&raw mut STREAM_BENCH) = Some(ove::stream!(STREAM_BUF_SIZE, 1));
        *(&raw mut STREAM_PRODUCER_TH) = Some(
            ove::thread!("strm_prod", stream_producer, Priority::Normal, 2048),
        );
    }
}

unsafe extern "C" fn stream_throughput_run(_ctx: *mut c_void) {
    unsafe {
        let _ = (*(&raw const STREAM_BENCH)).as_ref().unwrap().receive(&mut *(&raw mut STREAM_RX_BUF), WAIT_FOREVER);
    }
}

unsafe extern "C" fn stream_throughput_teardown(_ctx: *mut c_void) {
    STREAM_DONE.store(true, Ordering::Relaxed);
    // Drain so producer can unblock
    unsafe {
        let _ = (*(&raw const STREAM_BENCH)).as_ref().unwrap().receive(&mut *(&raw mut STREAM_RX_BUF), 100);
        Thread::sleep_ms(10);
        *(&raw mut STREAM_PRODUCER_TH) = None;
        *(&raw mut STREAM_BENCH) = None;
    }
}

// --- memory ---

static mut STREAM_MEM: Option<Stream<STREAM_BUF_SIZE>> = None;

unsafe extern "C" fn stream_memory_run(_ctx: *mut c_void) {
    unsafe { *(&raw mut STREAM_MEM) = Some(ove::stream!(STREAM_BUF_SIZE, 1)) };
}

unsafe extern "C" fn stream_memory_teardown(_ctx: *mut c_void) {
    unsafe { *(&raw mut STREAM_MEM) = None };
}

// --- stream suite ---

unsafe extern "C" fn stream_is_enabled() -> i32 {
    1
}

static STREAM_CASES: [BenchCase; 4] = [
    BenchCase {
        name: b"memory\0".as_ptr() as *const _,
        bench_type: BenchType::Memory,
        setup: NONE,
        run: Some(stream_memory_run),
        teardown: Some(stream_memory_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"send_recv_64B\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: Some(stream_send_recv_setup),
        run: Some(stream_send_recv_run),
        teardown: Some(stream_send_recv_teardown),
        iterations: 0,
    },
    BenchCase {
        name: b"create_destroy\0".as_ptr() as *const _,
        bench_type: BenchType::Latency,
        setup: NONE,
        run: Some(stream_create_destroy_run),
        teardown: NONE,
        iterations: 0,
    },
    BenchCase {
        name: b"throughput\0".as_ptr() as *const _,
        bench_type: BenchType::Throughput,
        setup: Some(stream_throughput_setup),
        run: Some(stream_throughput_run),
        teardown: Some(stream_throughput_teardown),
        iterations: 0,
    },
];

#[unsafe(no_mangle)]
pub static bench_suite_stream: BenchSuite = BenchSuite {
    name: b"stream\0".as_ptr() as *const _,
    is_enabled: Some(stream_is_enabled),
    cases: STREAM_CASES.as_ptr(),
    case_count: STREAM_CASES.len() as u32,
};

// ===========================================================================
// Suite registry & runner
// ===========================================================================

static SUITES: [&BenchSuite; 8] = [
    &bench_suite_time,
    &bench_suite_thread,
    &bench_suite_sync,
    &bench_suite_queue,
    &bench_suite_timer,
    &bench_suite_eventgroup,
    &bench_suite_workqueue,
    &bench_suite_stream,
];

fn benchmark_runner() {
    ove::log_inf!("=== oveRTOS Benchmark Suite ===");
    ove::log_inf!(
        "Iterations: {}  Warmup: {}",
        option_env!("OVE_BENCH_ITERATIONS").unwrap_or("1000"),
        option_env!("OVE_BENCH_WARMUP").unwrap_or("100")
    );

    for suite in &SUITES {
        let enabled = match suite.is_enabled {
            Some(f) => unsafe { f() },
            None => 0,
        };

        if enabled == 0 {
            // Log suite skipped -- extract the name for the message
            ove::log_inf!("Suite: SKIPPED (module disabled)");
            continue;
        }

        unsafe { bench_print_header(suite.name) };

        for c in 0..suite.case_count {
            let bc = unsafe { &*suite.cases.add(c as usize) };
            let mut result = BenchResult {
                min_ns: 0,
                max_ns: 0,
                total_ns: 0,
                count: 0,
                ops_per_sec: 0,
                heap_delta: -1,
            };

            unsafe {
                bench_run_case(bc as *const BenchCase, &mut result as *mut BenchResult);
                bench_print_result(bc as *const BenchCase, &result as *const BenchResult);
            }
        }

        unsafe { bench_print_footer() };
    }

    ove::log_inf!("=== Benchmark complete ===");
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log_inf!("Benchmark app: init");

    let _runner = ove::thread!("bench_run", benchmark_runner, Priority::Normal, 8192);

    ove::run();

    ove::log_inf!("Benchmark app: shutdown");
}

ove::main!(app_main);
