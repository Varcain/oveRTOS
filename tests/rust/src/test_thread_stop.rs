// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.
//
// Rust binding integration of cooperative cancellation (Phase 4 / 4).
// Exercises the Builder + JoinHandle restructure:
//   - ove::StopToken — read-only cancellation handle.
//   - Thread::builder().spawn(|tok| ...) — std::jthread analog,
//     stateful captures via FnOnce(StopToken) heap-boxed closure.
//   - .spawn_cooperative(fn(StopToken)) — stateless variant.
//   - .spawn_simple(fn()) — legacy entry, no token.
//   - JoinHandle::Drop calls request_stop then joins.
//   - JoinHandle::detach() opts out of join-on-drop.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Priority, StopToken, Thread};
use std::sync::atomic::{AtomicI32, Ordering};

static OBSERVED_FALSE_BEFORE: AtomicI32 = AtomicI32::new(0);
static OBSERVED_TRUE_AFTER: AtomicI32 = AtomicI32::new(0);
static EXITED: AtomicI32 = AtomicI32::new(0);

fn reset_flags() {
    OBSERVED_FALSE_BEFORE.store(0, Ordering::Release);
    OBSERVED_TRUE_AFTER.store(0, Ordering::Release);
    EXITED.store(0, Ordering::Release);
}

fn wait_flag(flag: &AtomicI32, expected: i32, timeout_ms: u32) -> bool {
    for _ in 0..timeout_ms {
        if flag.load(Ordering::Acquire) == expected {
            return true;
        }
        Thread::sleep_ms(1);
    }
    flag.load(Ordering::Acquire) == expected
}

fn cooperative_worker(tok: StopToken) {
    if !tok.is_stopped() {
        OBSERVED_FALSE_BEFORE.store(1, Ordering::Release);
    }
    while !tok.is_stopped() {
        Thread::sleep_ms(2);
    }
    OBSERVED_TRUE_AFTER.store(1, Ordering::Release);
    EXITED.store(1, Ordering::Release);
}

/// 1. Cooperative drop: out-of-scope JoinHandle sets stop flag then joins.
fn test_cooperative_drop_exits_cleanly() {
    reset_flags();
    {
        let _h = Thread::builder()
            .name(c"coop")
            .priority(Priority::Normal)
            .stack_size(4096)
            .spawn_cooperative(cooperative_worker)
            .expect("spawn_cooperative");
        assert!(
            wait_flag(&OBSERVED_FALSE_BEFORE, 1, 1000),
            "worker never observed false on entry"
        );
        // _h dropped here -> request_stop + join
    }
    assert_eq!(OBSERVED_TRUE_AFTER.load(Ordering::Acquire), 1);
    assert_eq!(EXITED.load(Ordering::Acquire), 1);
}

/// 2. Explicit request_stop callable on the JoinHandle.
fn test_explicit_request_stop() {
    reset_flags();
    {
        let h = Thread::builder()
            .name(c"coop")
            .priority(Priority::Normal)
            .stack_size(4096)
            .spawn_cooperative(cooperative_worker)
            .expect("spawn_cooperative");
        assert!(wait_flag(&OBSERVED_FALSE_BEFORE, 1, 1000));
        assert!(!h.stop_requested());
        h.request_stop();
        assert!(h.stop_requested());
    }
    assert_eq!(EXITED.load(Ordering::Acquire), 1);
}

/// 3. stop_token() returns a Send + Sync handle that flows into helpers.
fn helper_checks_token(tok: StopToken) -> bool {
    tok.stop_possible() && tok.is_stopped()
}

fn test_stop_token_shareable() {
    reset_flags();
    let h = Thread::builder()
        .name(c"coop")
        .priority(Priority::Normal)
        .stack_size(4096)
        .spawn_cooperative(cooperative_worker)
        .expect("spawn_cooperative");
    let tok = h.stop_token();
    assert!(tok.stop_possible());
    assert!(!helper_checks_token(tok));
    h.request_stop();
    assert!(helper_checks_token(tok));
}

/// 4. Stateful spawn (FnOnce(StopToken)) — captures flow in via heap-boxed closure.
fn test_spawn_captures() {
    use std::sync::Arc;
    use std::sync::atomic::AtomicU32;

    reset_flags();
    let counter = Arc::new(AtomicU32::new(0));
    let counter_clone = Arc::clone(&counter);
    {
        let _h = Thread::builder()
            .name(c"coopw")
            .priority(Priority::Normal)
            .stack_size(4096)
            .spawn(move |tok: StopToken| {
                while !tok.is_stopped() {
                    counter_clone.fetch_add(1, Ordering::Relaxed);
                    Thread::sleep_ms(2);
                }
                EXITED.store(1, Ordering::Release);
            })
            .expect("spawn");
        Thread::sleep_ms(20);
    }
    assert_eq!(EXITED.load(Ordering::Acquire), 1);
    assert!(
        counter.load(Ordering::Relaxed) > 0,
        "captured Arc was never incremented"
    );
}

/// 5. Empty StopToken: stop_possible() and is_stopped() are false.
fn test_empty_stop_token() {
    let tok = StopToken::empty();
    assert!(!tok.stop_possible());
    assert!(!tok.is_stopped());
}

/// 6. Legacy fn() entry via spawn_simple still works.
static LEGACY_RAN: AtomicI32 = AtomicI32::new(0);

fn legacy_entry() {
    LEGACY_RAN.store(1, Ordering::Release);
}

fn test_legacy_entry_unaffected() {
    LEGACY_RAN.store(0, Ordering::Release);
    {
        let _h = Thread::builder()
            .name(c"legacy")
            .priority(Priority::Normal)
            .stack_size(4096)
            .spawn_simple(legacy_entry)
            .expect("spawn_simple");
    }
    assert!(wait_flag(&LEGACY_RAN, 1, 500));
}

/// 7. Builder default fields (no explicit name/priority/stack_size) work.
fn test_builder_defaults() {
    reset_flags();
    {
        let _h = Thread::builder()
            .spawn_cooperative(cooperative_worker)
            .expect("spawn_cooperative with defaults");
        assert!(wait_flag(&OBSERVED_FALSE_BEFORE, 1, 1000));
    }
    assert_eq!(EXITED.load(Ordering::Acquire), 1);
}

pub fn run() -> (usize, usize) {
    run_suite("ThreadStop", &[
        test_entry!(test_cooperative_drop_exits_cleanly),
        test_entry!(test_explicit_request_stop),
        test_entry!(test_stop_token_shareable),
        test_entry!(test_spawn_captures),
        test_entry!(test_empty_stop_token),
        test_entry!(test_legacy_entry_unaffected),
        test_entry!(test_builder_defaults),
    ])
}
