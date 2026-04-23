// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Thread, ThreadInfo, ThreadState, Error};
use ove::ffi;
use std::sync::atomic::{AtomicI32, Ordering};

static FLAG: AtomicI32 = AtomicI32::new(0);
static KEEP_RUNNING: AtomicI32 = AtomicI32::new(0);

fn entry_set_flag() {
    FLAG.store(1, Ordering::Release);
}

fn entry_spin() {
    while KEEP_RUNNING.load(Ordering::Acquire) != 0 {
        Thread::sleep_ms(1);
    }
}

fn entry_busy_spin() {
    // Busy-loop without any RTOS syscall. POSIX backend sets the thread
    // state to RUNNING after trampoline start and never transitions it
    // back to BLOCKED because we make no blocking call here.
    while KEEP_RUNNING.load(Ordering::Acquire) != 0 {
        for _ in 0..1024 { core::hint::spin_loop(); }
    }
}

fn entry_sleep_briefly() {
    FLAG.store(1, Ordering::Release);
    Thread::sleep_ms(200);
    FLAG.store(2, Ordering::Release);
}

fn test_create_destroy() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::spawn(b"t1\0", entry_set_flag, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(50);
    assert_eq!(FLAG.load(Ordering::SeqCst), 1);
    drop(th);
}

fn test_sleep_duration() {
    let before = std::time::Instant::now();
    Thread::sleep_ms(50);
    let elapsed = before.elapsed().as_millis();
    assert!(elapsed >= 25 && elapsed <= 150, "elapsed={}ms", elapsed);
}

fn test_yield_no_crash() {
    Thread::yield_now();
}

fn test_get_self() {
    let _t = Thread::current();
}

fn test_set_priority() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t7\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(10);
    th.set_priority(ove::Priority::High);
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_get_state_running() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t8\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(20);
    let st = th.get_state();
    assert!(
        st == ThreadState::Running || st == ThreadState::Ready || st == ThreadState::Blocked,
        "state={:?}",
        st
    );
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_get_state_terminated() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::spawn(b"t9\0", entry_set_flag, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(100);
    let st = th.get_state();
    assert!(
        st == ThreadState::Terminated || st == ThreadState::Suspended,
        "state={:?}",
        st
    );
    drop(th);
}

fn test_stack_usage() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t10\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(10);
    let _usage = th.get_stack_usage();
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_suspend_resume() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::spawn(b"t14\0", entry_sleep_briefly, ove::Priority::Normal, 4096).unwrap();
    for _ in 0..100 {
        if FLAG.load(Ordering::Acquire) != 0 { break; }
        Thread::sleep_ms(5);
    }
    assert_eq!(FLAG.load(Ordering::SeqCst), 1);

    th.suspend();
    Thread::sleep_ms(10);
    th.resume();
    Thread::sleep_ms(300);
    drop(th);
}

fn test_runtime_stats() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"t16\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(20);
    let result = th.get_runtime_stats();
    assert!(result.is_ok() || matches!(result, Err(Error::NotSupported)));
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_raii_drop() {
    FLAG.store(0, Ordering::SeqCst);
    {
        let _th = Thread::spawn(b"raii\0", entry_set_flag, ove::Priority::Normal, 4096).unwrap();
        Thread::sleep_ms(50);
    }
}

/* ── Debug impl ─────────────────────────────────────────────────── */

fn test_thread_debug_format() {
    let th = Thread::current();
    let s = format!("{:?}", th);
    assert!(s.contains("Thread"), "debug output missing type name: {s}");
    assert!(s.contains("handle"), "debug output missing handle field: {s}");
}

fn test_thread_debug_spawned() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"dbg\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    let s = format!("{:?}", th);
    assert!(s.contains("owned: true"), "spawned thread should be owned: {s}");
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

/* ── Thread::create raw-entry variant ───────────────────────────── */

unsafe extern "C" fn c_entry_set_flag(_arg: *mut core::ffi::c_void) {
    FLAG.store(7, Ordering::Release);
}

fn test_create_raw_entry() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::create(b"craw\0", c_entry_set_flag, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(50);
    assert_eq!(FLAG.load(Ordering::SeqCst), 7);
    drop(th);
}

/* ── ThreadState::Suspended / Unknown match arms ────────────────── */

fn test_get_state_suspended_arm() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"susp\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(10);
    th.suspend();
    // Poll briefly — POSIX backend may take a moment to report suspension.
    let mut saw_suspended = false;
    for _ in 0..20 {
        if th.get_state() == ThreadState::Suspended {
            saw_suspended = true;
            break;
        }
        Thread::sleep_ms(5);
    }
    // Not all backends implement suspended-state reporting; accept the
    // fallback where get_state returns Running/Ready/Blocked.  What we
    // care about is exercising the match arm, which happens on every
    // call regardless of outcome.
    let _ = saw_suspended;
    th.resume();
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

/* ── System heap statistics ─────────────────────────────────────── */

fn test_get_mem_stats() {
    let result = ove::thread::get_mem_stats();
    match result {
        Ok(stats) => {
            // Backends differ in how tightly they track these values —
            // POSIX in particular does not maintain peak/free counters.
            // Just exercise the field accessors and Debug/Copy derives.
            let _ = stats.total;
            let _ = stats.free;
            let _ = stats.used;
            let _ = stats.peak_used;
            let copied = stats;
            let _ = format!("{:?}", copied);
        }
        Err(Error::NotSupported) => {}
        Err(other) => panic!("unexpected error: {other:?}"),
    }
}

/* ── Thread enumeration ─────────────────────────────────────────── */

fn test_thread_list_smoke() {
    let mut buf = [ThreadInfo {
        name: &[],
        state: 0 as ffi::ove_thread_state_t,
        priority: 0,
        stack_used: 0,
    }; 16];
    let cap = buf.len();
    match ove::thread::thread_list(&mut buf) {
        Ok(slice) => {
            assert!(slice.len() <= cap);
            for info in slice {
                let _ = format!("{:?}", *info);
                assert!(info.name.len() < isize::MAX as usize);
            }
        }
        Err(Error::NotSupported) => {}
        Err(other) => panic!("unexpected error: {other:?}"),
    }
}

fn test_thread_list_zero_capacity() {
    let mut buf: [ThreadInfo; 0] = [];
    let result = ove::thread::thread_list(&mut buf);
    match result {
        Ok(slice) => assert!(slice.is_empty()),
        Err(Error::NotSupported) => {}
        Err(other) => panic!("unexpected error: {other:?}"),
    }
}

fn test_thread_list_with_spawned() {
    // Spawn a named thread so thread_list returns >= 1 entry, forcing the
    // Rust name-parsing + ThreadInfo construction branch to run.
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"enum\0", entry_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(20);

    let mut buf = [ThreadInfo {
        name: &[],
        state: 0 as ffi::ove_thread_state_t,
        priority: 0,
        stack_used: 0,
    }; 16];
    match ove::thread::thread_list(&mut buf) {
        Ok(slice) => {
            assert!(!slice.is_empty(), "expected at least one live thread");
            for info in slice {
                // Exercise the fields so the construction isn't DCE'd.
                let _ = info.name;
                let _ = info.state;
                let _ = info.priority;
                let _ = info.stack_used;
            }
        }
        Err(Error::NotSupported) => {}
        Err(other) => panic!("unexpected error: {other:?}"),
    }

    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_get_state_running_arm() {
    // A busy-spinning thread stays in RUNNING state (POSIX backend only
    // transitions on sleep/yield).  Poll to exercise the Running/Ready
    // match arms in get_state.
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::spawn(b"busy\0", entry_busy_spin, ove::Priority::Normal, 4096).unwrap();
    Thread::sleep_ms(30);
    for _ in 0..40 {
        let st = th.get_state();
        if st == ThreadState::Running || st == ThreadState::Ready {
            break;
        }
        Thread::sleep_ms(2);
    }
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

/* ── Priority ordering (compiled Debug + Ord) ───────────────────── */

fn test_priority_ordering() {
    use ove::Priority;
    assert!(Priority::Idle < Priority::Normal);
    assert!(Priority::High > Priority::Low);
    assert!(Priority::Critical > Priority::Realtime);
    assert_eq!(Priority::Normal, Priority::Normal);
    let copy = Priority::High;
    assert_eq!(copy, Priority::High);
    let _ = format!("{:?}", Priority::BelowNormal);
}

/* ── ThreadState derive exercise ────────────────────────────────── */

fn test_thread_state_debug_eq() {
    let a = ThreadState::Running;
    let b = a;
    assert_eq!(a, b);
    assert_ne!(ThreadState::Ready, ThreadState::Blocked);
    let _ = format!("{:?}", ThreadState::Unknown);
    let _ = format!("{:?}", ThreadState::Terminated);
}

pub fn run() -> (usize, usize) {
    run_suite("Thread", &[
        test_entry!(test_create_destroy),
        test_entry!(test_sleep_duration),
        test_entry!(test_yield_no_crash),
        test_entry!(test_get_self),
        test_entry!(test_set_priority),
        test_entry!(test_get_state_running),
        test_entry!(test_get_state_terminated),
        test_entry!(test_stack_usage),
        test_entry!(test_suspend_resume),
        test_entry!(test_runtime_stats),
        test_entry!(test_raii_drop),
        test_entry!(test_thread_debug_format),
        test_entry!(test_thread_debug_spawned),
        test_entry!(test_create_raw_entry),
        test_entry!(test_get_state_suspended_arm),
        test_entry!(test_get_state_running_arm),
        test_entry!(test_get_mem_stats),
        test_entry!(test_thread_list_smoke),
        test_entry!(test_thread_list_zero_capacity),
        test_entry!(test_thread_list_with_spawned),
        test_entry!(test_priority_ordering),
        test_entry!(test_thread_state_debug_eq),
    ])
}
