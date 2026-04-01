// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Benchmark Application (Rust)
//!
//! Measures latency, throughput, and memory usage of all RTOS abstractions.
//! Suite symbols are exported as `#[no_mangle]` C-compatible statics so the
//! shared C harness (`bench_run_case`, `bench_print_*`) can drive them.

#![cfg_attr(not(feature = "std"), no_std)]

use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use ove::ffi;
use ove::Priority;

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
// FFI helpers
// ---------------------------------------------------------------------------

/// Null pointer constant for optional function pointers.
const NONE: Option<unsafe extern "C" fn(*mut c_void)> = None;

// ===========================================================================
// Suite: time
// ===========================================================================

unsafe extern "C" fn time_get_us_overhead_run(_ctx: *mut c_void) {
    let mut t: u64 = 0;
    unsafe { ffi::ove_time_get_us(&mut t) };
}

unsafe extern "C" fn delay_1ms_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_time_delay_ms(1) };
}

unsafe extern "C" fn time_is_enabled() -> i32 {
    #[cfg(has_time)]
    { 1 }
    #[cfg(not(has_time))]
    { 0 }
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

static mut THREAD_BENCH_TH: ffi::ove_thread_t = core::ptr::null_mut();
static mut THREAD_PING_SEM: ffi::ove_sem_t = core::ptr::null_mut();
static mut THREAD_PONG_SEM: ffi::ove_sem_t = core::ptr::null_mut();
static THREAD_CTX_SWITCH_DONE: AtomicBool = AtomicBool::new(false);

unsafe extern "C" fn dummy_thread(_arg: *mut c_void) {}

unsafe extern "C" fn thread_create_destroy_run(_ctx: *mut c_void) {
    let mut th: ffi::ove_thread_t = core::ptr::null_mut();
    let desc = ffi::ove_thread_desc {
        name: b"bench_tmp\0".as_ptr() as *const _,
        entry: Some(dummy_thread),
        arg: core::ptr::null_mut(),
        priority: Priority::Low as u32,
        stack_size: 1024,
        stack: core::ptr::null_mut(),
    };
    unsafe {
        ffi::ove_thread_create_(&mut th, &desc);
        ffi::ove_thread_destroy(th);
    }
}

unsafe extern "C" fn thread_yield_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_thread_yield() };
}

unsafe extern "C" fn thread_sleep_1ms_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_thread_sleep_ms(1) };
}

unsafe extern "C" fn pong_thread(_arg: *mut c_void) {
    while !THREAD_CTX_SWITCH_DONE.load(Ordering::Relaxed) {
        unsafe {
            ffi::ove_sem_take(THREAD_PING_SEM, ffi::OVE_WAIT_FOREVER);
            ffi::ove_sem_give(THREAD_PONG_SEM);
        }
    }
}

unsafe extern "C" fn ctx_switch_setup(_ctx: *mut c_void) {
    THREAD_CTX_SWITCH_DONE.store(false, Ordering::Relaxed);
    unsafe {
        ffi::ove_sem_create(&mut THREAD_PING_SEM, 0, 1);
        ffi::ove_sem_create(&mut THREAD_PONG_SEM, 0, 1);
    }
    let desc = ffi::ove_thread_desc {
        name: b"pong\0".as_ptr() as *const _,
        entry: Some(pong_thread),
        arg: core::ptr::null_mut(),
        priority: Priority::Normal as u32,
        stack_size: 2048,
        stack: core::ptr::null_mut(),
    };
    unsafe { ffi::ove_thread_create_(&mut THREAD_BENCH_TH, &desc) };
}

unsafe extern "C" fn ctx_switch_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_sem_give(THREAD_PING_SEM);
        ffi::ove_sem_take(THREAD_PONG_SEM, ffi::OVE_WAIT_FOREVER);
    }
}

unsafe extern "C" fn ctx_switch_teardown(_ctx: *mut c_void) {
    THREAD_CTX_SWITCH_DONE.store(true, Ordering::Relaxed);
    unsafe {
        ffi::ove_sem_give(THREAD_PING_SEM);
        ffi::ove_thread_sleep_ms(10);
        ffi::ove_thread_destroy(THREAD_BENCH_TH);
        ffi::ove_sem_destroy(THREAD_PING_SEM);
        ffi::ove_sem_destroy(THREAD_PONG_SEM);
    }
}

unsafe extern "C" fn thread_is_enabled() -> i32 {
    #[cfg(has_sync)]
    { 1 }
    #[cfg(not(has_sync))]
    { 0 }
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

static mut SYNC_MTX: ffi::ove_mutex_t = core::ptr::null_mut();
static mut SYNC_SEM: ffi::ove_sem_t = core::ptr::null_mut();
static mut SYNC_EVT: ffi::ove_event_t = core::ptr::null_mut();
static mut SYNC_CV: ffi::ove_condvar_t = core::ptr::null_mut();
static mut SYNC_CV_MTX: ffi::ove_mutex_t = core::ptr::null_mut();
static mut SYNC_RMTX: ffi::ove_mutex_t = core::ptr::null_mut();
static mut SYNC_CONTENTION_TH: ffi::ove_thread_t = core::ptr::null_mut();
static SYNC_CONTENTION_DONE: AtomicBool = AtomicBool::new(false);
static SYNC_CONTENTION_COUNT: AtomicU32 = AtomicU32::new(0);
static mut SYNC_EVT_TH: ffi::ove_thread_t = core::ptr::null_mut();
static SYNC_EVT_DONE: AtomicBool = AtomicBool::new(false);
static mut SYNC_CV_TH: ffi::ove_thread_t = core::ptr::null_mut();
static SYNC_CV_DONE: AtomicBool = AtomicBool::new(false);

// --- Mutex lock/unlock ---

unsafe extern "C" fn mutex_lock_unlock_setup(_ctx: *mut c_void) {
    unsafe { ffi::ove_mutex_create(&mut SYNC_MTX) };
}

unsafe extern "C" fn mutex_lock_unlock_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_mutex_lock(SYNC_MTX, ffi::OVE_WAIT_FOREVER);
        ffi::ove_mutex_unlock(SYNC_MTX);
    }
}

unsafe extern "C" fn mutex_lock_unlock_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_mutex_destroy(SYNC_MTX) };
}

// --- Mutex create/destroy ---

unsafe extern "C" fn mutex_create_destroy_run(_ctx: *mut c_void) {
    let mut m: ffi::ove_mutex_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_mutex_create(&mut m);
        ffi::ove_mutex_destroy(m);
    }
}

// --- Mutex contention (2-thread throughput) ---

unsafe extern "C" fn contention_thread(_arg: *mut c_void) {
    while !SYNC_CONTENTION_DONE.load(Ordering::Relaxed) {
        unsafe {
            ffi::ove_mutex_lock(SYNC_MTX, ffi::OVE_WAIT_FOREVER);
        }
        SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
        unsafe {
            ffi::ove_mutex_unlock(SYNC_MTX);
        }
    }
}

unsafe extern "C" fn mutex_contention_setup(_ctx: *mut c_void) {
    SYNC_CONTENTION_DONE.store(false, Ordering::Relaxed);
    SYNC_CONTENTION_COUNT.store(0, Ordering::Relaxed);
    unsafe { ffi::ove_mutex_create(&mut SYNC_MTX) };
    let desc = ffi::ove_thread_desc {
        name: b"contention\0".as_ptr() as *const _,
        entry: Some(contention_thread),
        arg: core::ptr::null_mut(),
        priority: Priority::Normal as u32,
        stack_size: 2048,
        stack: core::ptr::null_mut(),
    };
    unsafe { ffi::ove_thread_create_(&mut SYNC_CONTENTION_TH, &desc) };
}

unsafe extern "C" fn mutex_contention_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_mutex_lock(SYNC_MTX, ffi::OVE_WAIT_FOREVER);
    }
    SYNC_CONTENTION_COUNT.fetch_add(1, Ordering::Relaxed);
    unsafe {
        ffi::ove_mutex_unlock(SYNC_MTX);
    }
}

unsafe extern "C" fn mutex_contention_teardown(_ctx: *mut c_void) {
    SYNC_CONTENTION_DONE.store(true, Ordering::Relaxed);
    unsafe {
        ffi::ove_thread_sleep_ms(10);
        ffi::ove_thread_destroy(SYNC_CONTENTION_TH);
        ffi::ove_mutex_destroy(SYNC_MTX);
    }
}

// --- Mutex memory ---

static mut SYNC_MEM_MUTEX: ffi::ove_mutex_t = core::ptr::null_mut();

unsafe extern "C" fn mutex_memory_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_mutex_create(&mut SYNC_MEM_MUTEX) };
}

unsafe extern "C" fn mutex_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_mutex_destroy(SYNC_MEM_MUTEX) };
}

// --- Semaphore take/give ---

unsafe extern "C" fn sem_take_give_setup(_ctx: *mut c_void) {
    unsafe { ffi::ove_sem_create(&mut SYNC_SEM, 1, 1) };
}

unsafe extern "C" fn sem_take_give_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_sem_take(SYNC_SEM, ffi::OVE_WAIT_FOREVER);
        ffi::ove_sem_give(SYNC_SEM);
    }
}

unsafe extern "C" fn sem_take_give_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_sem_destroy(SYNC_SEM) };
}

// --- Semaphore create/destroy ---

unsafe extern "C" fn sem_create_destroy_run(_ctx: *mut c_void) {
    let mut s: ffi::ove_sem_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_sem_create(&mut s, 0, 1);
        ffi::ove_sem_destroy(s);
    }
}

// --- Semaphore memory ---

static mut SYNC_MEM_SEM: ffi::ove_sem_t = core::ptr::null_mut();

unsafe extern "C" fn sem_memory_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_sem_create(&mut SYNC_MEM_SEM, 0, 1) };
}

unsafe extern "C" fn sem_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_sem_destroy(SYNC_MEM_SEM) };
}

// --- Event signal/wait ---

unsafe extern "C" fn evt_signaler(_arg: *mut c_void) {
    while !SYNC_EVT_DONE.load(Ordering::Relaxed) {
        unsafe {
            ffi::ove_event_signal(SYNC_EVT);
            ffi::ove_thread_yield();
        }
    }
}

unsafe extern "C" fn event_signal_wait_setup(_ctx: *mut c_void) {
    SYNC_EVT_DONE.store(false, Ordering::Relaxed);
    unsafe { ffi::ove_event_create(&mut SYNC_EVT) };
    let desc = ffi::ove_thread_desc {
        name: b"evt_sig\0".as_ptr() as *const _,
        entry: Some(evt_signaler),
        arg: core::ptr::null_mut(),
        priority: Priority::Normal as u32,
        stack_size: 1024,
        stack: core::ptr::null_mut(),
    };
    unsafe { ffi::ove_thread_create_(&mut SYNC_EVT_TH, &desc) };
}

unsafe extern "C" fn event_signal_wait_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_event_wait(SYNC_EVT, 10) };
}

unsafe extern "C" fn event_signal_wait_teardown(_ctx: *mut c_void) {
    SYNC_EVT_DONE.store(true, Ordering::Relaxed);
    unsafe {
        ffi::ove_thread_sleep_ms(10);
        ffi::ove_thread_destroy(SYNC_EVT_TH);
        ffi::ove_event_destroy(SYNC_EVT);
    }
}

// --- Event memory ---

static mut SYNC_MEM_EVENT: ffi::ove_event_t = core::ptr::null_mut();

unsafe extern "C" fn event_memory_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_event_create(&mut SYNC_MEM_EVENT) };
}

unsafe extern "C" fn event_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_event_destroy(SYNC_MEM_EVENT) };
}

// --- Condvar signal/wait ---

unsafe extern "C" fn cv_signaler(_arg: *mut c_void) {
    while !SYNC_CV_DONE.load(Ordering::Relaxed) {
        unsafe {
            ffi::ove_condvar_signal(SYNC_CV);
            ffi::ove_thread_yield();
        }
    }
}

unsafe extern "C" fn condvar_signal_wait_setup(_ctx: *mut c_void) {
    SYNC_CV_DONE.store(false, Ordering::Relaxed);
    unsafe {
        ffi::ove_mutex_create(&mut SYNC_CV_MTX);
        ffi::ove_condvar_create(&mut SYNC_CV);
    }
    let desc = ffi::ove_thread_desc {
        name: b"cv_sig\0".as_ptr() as *const _,
        entry: Some(cv_signaler),
        arg: core::ptr::null_mut(),
        priority: Priority::Normal as u32,
        stack_size: 1024,
        stack: core::ptr::null_mut(),
    };
    unsafe { ffi::ove_thread_create_(&mut SYNC_CV_TH, &desc) };
}

unsafe extern "C" fn condvar_signal_wait_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_mutex_lock(SYNC_CV_MTX, ffi::OVE_WAIT_FOREVER);
        ffi::ove_condvar_wait(SYNC_CV, SYNC_CV_MTX, 10);
        ffi::ove_mutex_unlock(SYNC_CV_MTX);
    }
}

unsafe extern "C" fn condvar_signal_wait_teardown(_ctx: *mut c_void) {
    SYNC_CV_DONE.store(true, Ordering::Relaxed);
    unsafe {
        ffi::ove_condvar_signal(SYNC_CV);
        ffi::ove_thread_sleep_ms(10);
        ffi::ove_thread_destroy(SYNC_CV_TH);
        ffi::ove_condvar_destroy(SYNC_CV);
        ffi::ove_mutex_destroy(SYNC_CV_MTX);
    }
}

// --- Condvar memory ---

static mut SYNC_MEM_CONDVAR: ffi::ove_condvar_t = core::ptr::null_mut();

unsafe extern "C" fn condvar_memory_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_condvar_create(&mut SYNC_MEM_CONDVAR) };
}

unsafe extern "C" fn condvar_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_condvar_destroy(SYNC_MEM_CONDVAR) };
}

// --- Recursive mutex lock/unlock ---

unsafe extern "C" fn rmtx_lock_unlock_setup(_ctx: *mut c_void) {
    unsafe { ffi::ove_recursive_mutex_create(&mut SYNC_RMTX) };
}

unsafe extern "C" fn rmtx_lock_unlock_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_recursive_mutex_lock(SYNC_RMTX, ffi::OVE_WAIT_FOREVER);
        ffi::ove_recursive_mutex_unlock(SYNC_RMTX);
    }
}

unsafe extern "C" fn rmtx_lock_unlock_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_recursive_mutex_destroy(SYNC_RMTX) };
}

// --- sync suite ---

unsafe extern "C" fn sync_is_enabled() -> i32 {
    #[cfg(has_sync)]
    { 1 }
    #[cfg(not(has_sync))]
    { 0 }
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

static mut QUEUE_Q: ffi::ove_queue_t = core::ptr::null_mut();
static mut QUEUE_PRODUCER_TH: ffi::ove_thread_t = core::ptr::null_mut();
static QUEUE_THROUGHPUT_DONE: AtomicBool = AtomicBool::new(false);

// --- send/receive latency ---

unsafe extern "C" fn queue_send_recv_setup(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_queue_create(
            &mut QUEUE_Q,
            core::mem::size_of::<u32>(),
            16,
        )
    };
}

unsafe extern "C" fn queue_send_recv_run(_ctx: *mut c_void) {
    let val: u32 = 42;
    let mut buf: u32 = 0;
    unsafe {
        ffi::ove_queue_send(QUEUE_Q, &val as *const u32 as *const _, ffi::OVE_WAIT_FOREVER);
        ffi::ove_queue_receive(QUEUE_Q, &mut buf as *mut u32 as *mut _, ffi::OVE_WAIT_FOREVER);
    }
}

unsafe extern "C" fn queue_send_recv_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_queue_destroy(QUEUE_Q) };
}

// --- create/destroy ---

unsafe extern "C" fn queue_create_destroy_run(_ctx: *mut c_void) {
    let mut q: ffi::ove_queue_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_queue_create(&mut q, core::mem::size_of::<u32>(), 8);
        ffi::ove_queue_destroy(q);
    }
}

// --- 2-thread throughput ---

unsafe extern "C" fn producer_thread(_arg: *mut c_void) {
    let mut val: u32 = 0;
    while !QUEUE_THROUGHPUT_DONE.load(Ordering::Relaxed) {
        unsafe {
            ffi::ove_queue_send(QUEUE_Q, &val as *const u32 as *const _, ffi::OVE_WAIT_FOREVER);
        }
        val = val.wrapping_add(1);
    }
}

unsafe extern "C" fn queue_throughput_setup(_ctx: *mut c_void) {
    QUEUE_THROUGHPUT_DONE.store(false, Ordering::Relaxed);
    unsafe {
        ffi::ove_queue_create(
            &mut QUEUE_Q,
            core::mem::size_of::<u32>(),
            64,
        );
    }
    let desc = ffi::ove_thread_desc {
        name: b"q_prod\0".as_ptr() as *const _,
        entry: Some(producer_thread),
        arg: core::ptr::null_mut(),
        priority: Priority::Normal as u32,
        stack_size: 2048,
        stack: core::ptr::null_mut(),
    };
    unsafe { ffi::ove_thread_create_(&mut QUEUE_PRODUCER_TH, &desc) };
}

unsafe extern "C" fn queue_throughput_run(_ctx: *mut c_void) {
    let mut buf: u32 = 0;
    unsafe {
        ffi::ove_queue_receive(QUEUE_Q, &mut buf as *mut u32 as *mut _, ffi::OVE_WAIT_FOREVER);
    }
}

unsafe extern "C" fn queue_throughput_teardown(_ctx: *mut c_void) {
    QUEUE_THROUGHPUT_DONE.store(true, Ordering::Relaxed);
    // Drain queue so producer unblocks
    let mut buf: u32 = 0;
    unsafe {
        ffi::ove_queue_receive(QUEUE_Q, &mut buf as *mut u32 as *mut _, 100);
        ffi::ove_thread_sleep_ms(10);
        ffi::ove_thread_destroy(QUEUE_PRODUCER_TH);
        ffi::ove_queue_destroy(QUEUE_Q);
    }
}

// --- memory ---

static mut QUEUE_MEM_Q: ffi::ove_queue_t = core::ptr::null_mut();

unsafe extern "C" fn queue_memory_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_queue_create(
            &mut QUEUE_MEM_Q,
            core::mem::size_of::<u32>(),
            8,
        )
    };
}

unsafe extern "C" fn queue_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_queue_destroy(QUEUE_MEM_Q) };
}

// --- queue suite ---

unsafe extern "C" fn queue_is_enabled() -> i32 {
    #[cfg(has_queue)]
    { 1 }
    #[cfg(not(has_queue))]
    { 0 }
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

static mut TIMER_TMR: ffi::ove_timer_t = core::ptr::null_mut();

unsafe extern "C" fn timer_dummy_cb(
    _timer: ffi::ove_timer_t,
    _user_data: *mut c_void,
) {
}

// --- create/destroy ---

unsafe extern "C" fn timer_create_destroy_run(_ctx: *mut c_void) {
    let mut t: ffi::ove_timer_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_timer_create(&mut t, Some(timer_dummy_cb), core::ptr::null_mut(), 1000, 0);
        ffi::ove_timer_destroy(t);
    }
}

// --- start/stop ---

unsafe extern "C" fn timer_start_stop_setup(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_timer_create(
            &mut TIMER_TMR,
            Some(timer_dummy_cb),
            core::ptr::null_mut(),
            1000,
            0,
        )
    };
}

unsafe extern "C" fn timer_start_stop_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_timer_start(TIMER_TMR);
        ffi::ove_timer_stop(TIMER_TMR);
    }
}

unsafe extern "C" fn timer_start_stop_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_timer_destroy(TIMER_TMR) };
}

// --- memory ---

static mut TIMER_MEM_TMR: ffi::ove_timer_t = core::ptr::null_mut();

unsafe extern "C" fn timer_memory_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_timer_create(
            &mut TIMER_MEM_TMR,
            Some(timer_dummy_cb),
            core::ptr::null_mut(),
            1000,
            0,
        )
    };
}

unsafe extern "C" fn timer_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_timer_destroy(TIMER_MEM_TMR) };
}

// --- timer suite ---

unsafe extern "C" fn timer_is_enabled() -> i32 {
    #[cfg(has_timer)]
    { 1 }
    #[cfg(not(has_timer))]
    { 0 }
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

static mut EG_BENCH: ffi::ove_eventgroup_t = core::ptr::null_mut();

// --- set/get bits ---

unsafe extern "C" fn eg_set_get_setup(_ctx: *mut c_void) {
    unsafe { ffi::ove_eventgroup_create(&mut EG_BENCH) };
}

unsafe extern "C" fn eg_set_get_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_eventgroup_set_bits(EG_BENCH, 0x01);
        ffi::ove_eventgroup_get_bits(EG_BENCH);
        ffi::ove_eventgroup_clear_bits(EG_BENCH, 0x01);
    }
}

unsafe extern "C" fn eg_set_get_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_eventgroup_destroy(EG_BENCH) };
}

// --- create/destroy ---

unsafe extern "C" fn eg_create_destroy_run(_ctx: *mut c_void) {
    let mut eg: ffi::ove_eventgroup_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_eventgroup_create(&mut eg);
        ffi::ove_eventgroup_destroy(eg);
    }
}

// --- memory ---

static mut EG_MEM: ffi::ove_eventgroup_t = core::ptr::null_mut();

unsafe extern "C" fn eg_memory_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_eventgroup_create(&mut EG_MEM) };
}

unsafe extern "C" fn eg_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_eventgroup_destroy(EG_MEM) };
}

// --- eventgroup suite ---

unsafe extern "C" fn eventgroup_is_enabled() -> i32 {
    #[cfg(has_eventgroup)]
    { 1 }
    #[cfg(not(has_eventgroup))]
    { 0 }
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

static mut WQ_BENCH: ffi::ove_workqueue_t = core::ptr::null_mut();
static mut WQ_WORK: ffi::ove_work_t = core::ptr::null_mut();
static WQ_WORK_EXECUTED: AtomicBool = AtomicBool::new(false);
static mut WQ_WORK_SEM: ffi::ove_sem_t = core::ptr::null_mut();
static mut WQ_WORK_STORAGE: core::mem::MaybeUninit<ffi::ove_work_storage_t> =
    core::mem::MaybeUninit::uninit();

unsafe extern "C" fn work_handler(_work: ffi::ove_work_t) {
    WQ_WORK_EXECUTED.store(true, Ordering::Relaxed);
    unsafe { ffi::ove_sem_give(WQ_WORK_SEM) };
}

// --- create/destroy ---

unsafe extern "C" fn wq_create_destroy_run(_ctx: *mut c_void) {
    let mut wq: ffi::ove_workqueue_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_workqueue_create(
            &mut wq,
            b"bench_wq\0".as_ptr() as *const _,
            Priority::Normal as u32,
            2048,
        );
        ffi::ove_workqueue_destroy(wq);
    }
}

// --- submit/execute ---

unsafe extern "C" fn wq_submit_setup(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_sem_create(&mut WQ_WORK_SEM, 0, 1);
        ffi::ove_workqueue_create(
            &mut WQ_BENCH,
            b"bench_wq\0".as_ptr() as *const _,
            Priority::Normal as u32,
            2048,
        );
        ffi::ove_work_init_static(
            &mut WQ_WORK,
            WQ_WORK_STORAGE.as_mut_ptr(),
            Some(work_handler),
        );
    }
}

unsafe extern "C" fn wq_submit_run(_ctx: *mut c_void) {
    WQ_WORK_EXECUTED.store(false, Ordering::Relaxed);
    unsafe {
        ffi::ove_work_submit(WQ_BENCH, WQ_WORK);
        ffi::ove_sem_take(WQ_WORK_SEM, 1000);
    }
}

unsafe extern "C" fn wq_submit_teardown(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_workqueue_destroy(WQ_BENCH);
        ffi::ove_sem_destroy(WQ_WORK_SEM);
    }
}

// --- memory ---

static mut WQ_MEM: ffi::ove_workqueue_t = core::ptr::null_mut();

unsafe extern "C" fn wq_memory_run(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_workqueue_create(
            &mut WQ_MEM,
            b"bench_wq\0".as_ptr() as *const _,
            Priority::Normal as u32,
            2048,
        )
    };
}

unsafe extern "C" fn wq_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_workqueue_destroy(WQ_MEM) };
}

// --- workqueue suite ---

unsafe extern "C" fn workqueue_is_enabled() -> i32 {
    #[cfg(has_workqueue)]
    { 1 }
    #[cfg(not(has_workqueue))]
    { 0 }
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

static mut STREAM_BENCH: ffi::ove_stream_t = core::ptr::null_mut();
static mut STREAM_PRODUCER_TH: ffi::ove_thread_t = core::ptr::null_mut();
static STREAM_DONE: AtomicBool = AtomicBool::new(false);

static mut STREAM_TX_BUF: [u8; STREAM_MSG_SIZE] = [0u8; STREAM_MSG_SIZE];
static mut STREAM_RX_BUF: [u8; STREAM_MSG_SIZE] = [0u8; STREAM_MSG_SIZE];

// --- send/receive 64B ---

unsafe extern "C" fn stream_send_recv_setup(_ctx: *mut c_void) {
    unsafe {
        ffi::ove_stream_create(&mut STREAM_BENCH, STREAM_BUF_SIZE, 1);
        STREAM_TX_BUF = [0xAA; STREAM_MSG_SIZE];
    }
}

unsafe extern "C" fn stream_send_recv_run(_ctx: *mut c_void) {
    let mut sent: usize = 0;
    let mut received: usize = 0;
    unsafe {
        ffi::ove_stream_send(
            STREAM_BENCH,
            STREAM_TX_BUF.as_ptr() as *const _,
            STREAM_MSG_SIZE,
            ffi::OVE_WAIT_FOREVER,
            &mut sent,
        );
        ffi::ove_stream_receive(
            STREAM_BENCH,
            STREAM_RX_BUF.as_mut_ptr() as *mut _,
            STREAM_MSG_SIZE,
            ffi::OVE_WAIT_FOREVER,
            &mut received,
        );
    }
}

unsafe extern "C" fn stream_send_recv_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_stream_destroy(STREAM_BENCH) };
}

// --- create/destroy ---

unsafe extern "C" fn stream_create_destroy_run(_ctx: *mut c_void) {
    let mut s: ffi::ove_stream_t = core::ptr::null_mut();
    unsafe {
        ffi::ove_stream_create(&mut s, STREAM_BUF_SIZE, 1);
        ffi::ove_stream_destroy(s);
    }
}

// --- throughput ---

unsafe extern "C" fn stream_producer(_arg: *mut c_void) {
    while !STREAM_DONE.load(Ordering::Relaxed) {
        let mut sent: usize = 0;
        unsafe {
            ffi::ove_stream_send(
                STREAM_BENCH,
                STREAM_TX_BUF.as_ptr() as *const _,
                STREAM_MSG_SIZE,
                ffi::OVE_WAIT_FOREVER,
                &mut sent,
            );
        }
    }
}

unsafe extern "C" fn stream_throughput_setup(_ctx: *mut c_void) {
    STREAM_DONE.store(false, Ordering::Relaxed);
    unsafe {
        STREAM_TX_BUF = [0xBB; STREAM_MSG_SIZE];
        ffi::ove_stream_create(&mut STREAM_BENCH, STREAM_BUF_SIZE, 1);
    }
    let desc = ffi::ove_thread_desc {
        name: b"strm_prod\0".as_ptr() as *const _,
        entry: Some(stream_producer),
        arg: core::ptr::null_mut(),
        priority: Priority::Normal as u32,
        stack_size: 2048,
        stack: core::ptr::null_mut(),
    };
    unsafe { ffi::ove_thread_create_(&mut STREAM_PRODUCER_TH, &desc) };
}

unsafe extern "C" fn stream_throughput_run(_ctx: *mut c_void) {
    let mut received: usize = 0;
    unsafe {
        ffi::ove_stream_receive(
            STREAM_BENCH,
            STREAM_RX_BUF.as_mut_ptr() as *mut _,
            STREAM_MSG_SIZE,
            ffi::OVE_WAIT_FOREVER,
            &mut received,
        );
    }
}

unsafe extern "C" fn stream_throughput_teardown(_ctx: *mut c_void) {
    STREAM_DONE.store(true, Ordering::Relaxed);
    // Drain so producer can unblock
    let mut received: usize = 0;
    unsafe {
        ffi::ove_stream_receive(
            STREAM_BENCH,
            STREAM_RX_BUF.as_mut_ptr() as *mut _,
            STREAM_MSG_SIZE,
            100,
            &mut received,
        );
        ffi::ove_thread_sleep_ms(10);
        ffi::ove_thread_destroy(STREAM_PRODUCER_TH);
        ffi::ove_stream_destroy(STREAM_BENCH);
    }
}

// --- memory ---

static mut STREAM_MEM: ffi::ove_stream_t = core::ptr::null_mut();

unsafe extern "C" fn stream_memory_run(_ctx: *mut c_void) {
    unsafe { ffi::ove_stream_create(&mut STREAM_MEM, STREAM_BUF_SIZE, 1) };
}

unsafe extern "C" fn stream_memory_teardown(_ctx: *mut c_void) {
    unsafe { ffi::ove_stream_destroy(STREAM_MEM) };
}

// --- stream suite ---

unsafe extern "C" fn stream_is_enabled() -> i32 {
    #[cfg(has_stream)]
    { 1 }
    #[cfg(not(has_stream))]
    { 0 }
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
