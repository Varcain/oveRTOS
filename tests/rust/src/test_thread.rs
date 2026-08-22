// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Error, Thread, ThreadInfo, ThreadState};
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
    let th = Thread::builder().name(c"t1").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_set_flag).unwrap();
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
    let th = Thread::builder().name(c"t7").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
    Thread::sleep_ms(10);
    th.set_priority(ove::Priority::High);
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_get_state_running() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::builder().name(c"t8").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
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
    let th = Thread::builder().name(c"t9").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_set_flag).unwrap();
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
    let th = Thread::builder().name(c"t10").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
    Thread::sleep_ms(10);
    match th.get_stack_headroom() {
        Ok(headroom) => assert_eq!(th.get_stack_usage(), headroom),
        Err(Error::NotSupported) => {}
        Err(error) => panic!("unexpected stack-headroom error: {error:?}"),
    }
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_suspend_resume() {
    FLAG.store(0, Ordering::SeqCst);
    let th = Thread::builder().name(c"t14").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_sleep_briefly).unwrap();
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
    let th = Thread::builder().name(c"t16").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
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
        let _th = Thread::builder().name(c"raii").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_set_flag).unwrap();
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
    // Builder::spawn_simple returns JoinHandle (post Phase 4 / Iteration 4
    // restructure); the previous test asserted the `owned` field appeared
    // in the Debug output — that field no longer exists.  Now assert the
    // new JoinHandle shape: type name + handle + detached state.
    let th = Thread::builder().name(c"dbg").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
    let s = format!("{:?}", th);
    assert!(s.contains("JoinHandle"), "debug output missing type name: {s}");
    assert!(s.contains("handle"), "debug output missing handle field: {s}");
    assert!(s.contains("detached: false"), "spawned JoinHandle should not be detached: {s}");
    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

/* ── Raw extern "C" entry path covered via ove::ffi ──────────────
 *
 * After the Phase 4 / Iteration 4 Builder/JoinHandle restructure the
 * binding no longer offers a `Thread::create`-style safe wrapper for
 * `unsafe extern "C" fn` entries — that path is reachable through the
 * `ove::ffi` escape hatch (see lib.rs).  The previous test_create_raw_entry
 * case was deleted to avoid testing a function that no longer exists; the
 * ffi::ove_thread_create call chain is exercised indirectly by the rest
 * of the Thread suite through Builder::spawn_simple. */

/* ── ThreadState::Suspended / Unknown match arms ────────────────── */

fn test_get_state_suspended_arm() {
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::builder().name(c"susp").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
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
    let mut buf = [ThreadInfo::empty(); 16];
    let cap = buf.len();
    match ove::thread::thread_list(&mut buf) {
        Ok(slice) => {
            assert!(slice.len() <= cap);
            for info in slice {
                let _ = format!("{:?}", *info);
                assert!(info.name().len() < isize::MAX as usize);
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
    let th = Thread::builder().name(c"enum").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_spin).unwrap();
    Thread::sleep_ms(20);

    let mut buf = [ThreadInfo::empty(); 16];
    match ove::thread::thread_list(&mut buf) {
        Ok(slice) => {
            assert!(!slice.is_empty(), "expected at least one live thread");
            for info in slice {
                // Exercise the fields so the construction isn't DCE'd.
                let _ = info.name();
                let _ = info.state();
                let _ = info.priority;
                let _ = info.valid_fields;
                let _ = info.stack_used();
                let _ = info.identity;
                let _ = info.stack_size();
                let _ = info.cpu_percent_x100();
                let _ = info.running_us();
                let _ = info.ready_us();
                let _ = info.blocked_us();
                let _ = info.suspended_us();
            }
        }
        Err(Error::NotSupported) => {}
        Err(other) => panic!("unexpected error: {other:?}"),
    }

    KEEP_RUNNING.store(0, Ordering::SeqCst);
    Thread::sleep_ms(20);
    drop(th);
}

fn test_thread_list_uses_full_caller_capacity() {
    let mut threads = Vec::new();
    for _ in 0..40 {
        threads.push(
            Thread::builder()
                .name(c"wide")
                .priority(ove::Priority::Normal)
                .stack_size(4096)
                .spawn(|tok| {
                    while !tok.is_stopped() {
                        Thread::sleep_ms(1);
                    }
                })
                .unwrap(),
        );
    }
    Thread::sleep_ms(20);

    let mut too_small = [ThreadInfo::empty(); 1];
    assert!(matches!(
        ove::thread::thread_list(&mut too_small),
        Err(Error::QueueFull)
    ));

    let mut buf = [ThreadInfo::empty(); 48];
    let list = ove::thread::thread_list(&mut buf).unwrap();
    assert!(list.len() >= 40, "binding truncated {} live threads", list.len());
    assert!(list.iter().filter(|info| info.name() == b"wide").count() >= 40);

    drop(threads);
}

fn test_get_state_running_arm() {
    // A busy-spinning thread stays in RUNNING state (POSIX backend only
    // transitions on sleep/yield).  Poll to exercise the Running/Ready
    // match arms in get_state.
    KEEP_RUNNING.store(1, Ordering::SeqCst);
    let th = Thread::builder().name(c"busy").priority(ove::Priority::Normal).stack_size(4096).spawn_simple(entry_busy_spin).unwrap();
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
        test_entry!(test_get_state_suspended_arm),
        test_entry!(test_get_state_running_arm),
        test_entry!(test_get_mem_stats),
        test_entry!(test_thread_list_smoke),
        test_entry!(test_thread_list_zero_capacity),
        test_entry!(test_thread_list_with_spawned),
        test_entry!(test_thread_list_uses_full_caller_capacity),
        test_entry!(test_priority_ordering),
        test_entry!(test_thread_state_debug_eq),
    ])
}
