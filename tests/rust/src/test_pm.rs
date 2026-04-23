// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::pm::{
    Cfg, Domain, Event, NotifyHandler, PolicyCtx, PolicyHandler, State, WakeType,
};
use ove::StaticCell;

/* PM is a singleton — `deinit` first so each test runs from a known state. */
fn fresh_init() {
    ove::pm::deinit();
    let cfg = Cfg {
        idle_threshold_ms: 10,
        standby_threshold_ms: 100,
        deep_sleep_threshold_ms: 1000,
    };
    ove::pm::init(&cfg).unwrap();
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

fn test_pm_init_deinit() {
    fresh_init();
    ove::pm::deinit();
}

fn test_pm_deinit_idempotent() {
    ove::pm::deinit();
    ove::pm::deinit(); // second call must not crash
}

fn test_pm_double_init_fails() {
    fresh_init();
    let cfg = Cfg {
        idle_threshold_ms: 10,
        standby_threshold_ms: 100,
        deep_sleep_threshold_ms: 1000,
    };
    assert!(ove::pm::init(&cfg).is_err());
    ove::pm::deinit();
}

/* ── State machine ──────────────────────────────────────────────────── */

fn test_pm_get_state_after_init() {
    fresh_init();
    assert_eq!(ove::pm::get_state(), State::Active);
    ove::pm::deinit();
}

fn test_pm_set_state_transitions() {
    fresh_init();
    ove::pm::set_state(State::Idle).unwrap();
    assert_eq!(ove::pm::get_state(), State::Idle);
    ove::pm::set_state(State::Standby).unwrap();
    assert_eq!(ove::pm::get_state(), State::Standby);
    ove::pm::set_state(State::DeepSleep).unwrap();
    assert_eq!(ove::pm::get_state(), State::DeepSleep);
    ove::pm::set_state(State::Active).unwrap();
    assert_eq!(ove::pm::get_state(), State::Active);
    ove::pm::deinit();
}

fn test_pm_set_state_same_is_ok() {
    fresh_init();
    ove::pm::set_state(State::Active).unwrap();
    ove::pm::set_state(State::Active).unwrap();
    ove::pm::deinit();
}

fn test_pm_set_state_without_init_errors() {
    ove::pm::deinit();
    assert!(ove::pm::set_state(State::Idle).is_err());
}

fn test_pm_activity() {
    fresh_init();
    // Activity is ISR-safe — just exercise the path, effects are observed
    // by the idle-processing loop which the stub doesn't drive.
    ove::pm::activity();
    ove::pm::activity();
    ove::pm::deinit();
}

/* ── Wake sources ───────────────────────────────────────────────────── */

fn test_pm_wake_register_gpio() {
    fresh_init();
    ove::pm::wake_register_gpio(0, 1, 0).unwrap();
    ove::pm::wake_unregister_gpio(0, 1).unwrap();
    ove::pm::deinit();
}

fn test_pm_wake_register_timer() {
    fresh_init();
    ove::pm::wake_register_timer(500).unwrap();
    ove::pm::deinit();
}

fn test_pm_wake_register_uart() {
    fresh_init();
    ove::pm::wake_register_uart(0).unwrap();
    ove::pm::deinit();
}

fn test_pm_wake_unregister_unknown_errors() {
    fresh_init();
    // No GPIO registered — unregister must report NotRegistered.
    assert!(ove::pm::wake_unregister_gpio(5, 5).is_err());
    ove::pm::deinit();
}

fn test_pm_wake_register_table_full() {
    fresh_init();
    // Fill the 8-slot table, then one more must fail.
    for pin in 0..8 {
        ove::pm::wake_register_gpio(0, pin, 0).unwrap();
    }
    assert!(ove::pm::wake_register_gpio(0, 8, 0).is_err());
    ove::pm::deinit();
}

/* ── Peripheral power domains ───────────────────────────────────────── */

fn test_pm_domain_request_release() {
    fresh_init();
    ove::pm::domain_request(Domain::Audio).unwrap();
    assert_eq!(ove::pm::domain_get_refcount(Domain::Audio).unwrap(), 1);
    ove::pm::domain_request(Domain::Audio).unwrap();
    assert_eq!(ove::pm::domain_get_refcount(Domain::Audio).unwrap(), 2);
    ove::pm::domain_release(Domain::Audio).unwrap();
    ove::pm::domain_release(Domain::Audio).unwrap();
    assert_eq!(ove::pm::domain_get_refcount(Domain::Audio).unwrap(), 0);
    ove::pm::deinit();
}

fn test_pm_domain_release_underflow_errors() {
    fresh_init();
    assert!(ove::pm::domain_release(Domain::Radio).is_err());
    ove::pm::deinit();
}

fn test_pm_domain_all_variants() {
    fresh_init();
    for d in [
        Domain::Radio,
        Domain::Sensor,
        Domain::Display,
        Domain::Audio,
        Domain::Storage,
        Domain::Comms,
        Domain::User0,
        Domain::User1,
    ] {
        ove::pm::domain_request(d).unwrap();
        assert_eq!(ove::pm::domain_get_refcount(d).unwrap(), 1);
        ove::pm::domain_release(d).unwrap();
    }
    ove::pm::deinit();
}

/* ── Policy ─────────────────────────────────────────────────────────── */

#[derive(Copy, Clone)]
struct PolicyState {
    forced: State,
}
unsafe impl Send for PolicyState {}
unsafe impl Sync for PolicyState {}

static POLICY_CELL: StaticCell<PolicyState> = StaticCell::new();
static POLICY_HANDLER: PolicyHandler<PolicyState> =
    PolicyHandler::new(&POLICY_CELL, |s: &PolicyState, _ctx: PolicyCtx| s.forced);

fn test_pm_clear_policy() {
    fresh_init();
    ove::pm::clear_policy().unwrap();
    ove::pm::deinit();
}

fn test_pm_set_policy_typed() {
    fresh_init();
    POLICY_CELL.shutdown();
    POLICY_CELL.init(PolicyState { forced: State::Idle });

    ove::pm::set_policy(&POLICY_HANDLER).unwrap();
    ove::pm::clear_policy().unwrap();

    POLICY_CELL.shutdown();
    ove::pm::deinit();
}

fn test_pm_set_policy_raw() {
    fresh_init();
    // None clears the policy — exercise the raw path.
    unsafe {
        ove::pm::set_policy_raw(None, core::ptr::null_mut()).unwrap();
    }
    ove::pm::deinit();
}

/* ── Notifications ──────────────────────────────────────────────────── */

#[derive(Copy, Clone)]
struct NotifyState {
    _marker: u8,
}
unsafe impl Send for NotifyState {}
unsafe impl Sync for NotifyState {}

static NOTIFY_CELL: StaticCell<NotifyState> = StaticCell::new();
static NOTIFY_HANDLER: NotifyHandler<NotifyState> =
    NotifyHandler::new(&NOTIFY_CELL, |_s, _ev, _from, _to| {});

fn test_pm_notify_register_unregister() {
    fresh_init();
    NOTIFY_CELL.shutdown();
    NOTIFY_CELL.init(NotifyState { _marker: 0 });

    ove::pm::notify_register(&NOTIFY_HANDLER).unwrap();
    ove::pm::notify_unregister(&NOTIFY_HANDLER).unwrap();

    NOTIFY_CELL.shutdown();
    ove::pm::deinit();
}

fn test_pm_notify_unregister_unknown_errors() {
    fresh_init();
    NOTIFY_CELL.shutdown();
    NOTIFY_CELL.init(NotifyState { _marker: 0 });
    // Never registered — unregister must fail.
    assert!(ove::pm::notify_unregister(&NOTIFY_HANDLER).is_err());
    NOTIFY_CELL.shutdown();
    ove::pm::deinit();
}

unsafe extern "C" fn raw_notify_cb(
    _event: u32,
    _from: u32,
    _to: u32,
    _user: *mut core::ffi::c_void,
) {
}

fn test_pm_notify_register_raw() {
    fresh_init();
    unsafe {
        ove::pm::notify_register_raw(Some(raw_notify_cb), core::ptr::null_mut()).unwrap();
        ove::pm::notify_unregister_raw(Some(raw_notify_cb), core::ptr::null_mut()).unwrap();
    }
    ove::pm::deinit();
}

/* ── Statistics ─────────────────────────────────────────────────────── */

fn test_pm_get_stats() {
    fresh_init();
    let stats = ove::pm::get_stats().unwrap();
    // One transition into ACTIVE is recorded at init.
    assert!(stats.transition_count[State::Active as usize] >= 1);
    // active_pct_x100 is in hundredths (0..=10000).
    assert!(stats.active_pct_x100 <= 10000);
    ove::pm::deinit();
}

fn test_pm_get_stats_without_init_errors() {
    ove::pm::deinit();
    assert!(ove::pm::get_stats().is_err());
}

fn test_pm_reset_stats() {
    fresh_init();
    ove::pm::set_state(State::Idle).unwrap();
    ove::pm::set_state(State::Active).unwrap();
    ove::pm::reset_stats();
    let stats = ove::pm::get_stats().unwrap();
    // After reset, only the current (ACTIVE) state has a transition count.
    assert_eq!(stats.transition_count[State::Idle as usize], 0);
    assert_eq!(stats.transition_count[State::Standby as usize], 0);
    assert_eq!(stats.transition_count[State::DeepSleep as usize], 0);
    ove::pm::deinit();
}

/* ── Power budget ───────────────────────────────────────────────────── */

fn test_pm_budget() {
    fresh_init();
    ove::pm::set_budget(5000).unwrap();
    let actual = ove::pm::get_budget_status().unwrap();
    assert!(actual <= 10000);
    ove::pm::deinit();
}

fn test_pm_budget_without_init_errors() {
    ove::pm::deinit();
    assert!(ove::pm::set_budget(5000).is_err());
    assert!(ove::pm::get_budget_status().is_err());
}

/* ── Trampoline invocation via idle_process ─────────────────────────── */
//
// `ove_pm_idle_process` consults the registered policy and, if the recommended
// state is deeper than the current one, drives a full PRE_SLEEP → HAL enter →
// POST_WAKE cycle synchronously. That's the only path that exercises the
// typed-trampoline bodies in `pm.rs`.

fn test_pm_policy_trampoline_fires() {
    fresh_init();
    POLICY_CELL.shutdown();
    POLICY_CELL.init(PolicyState { forced: State::Idle });
    ove::pm::set_policy(&POLICY_HANDLER).unwrap();

    // current=Active, policy returns Idle → transition, PRE_SLEEP + POST_WAKE
    // fire through the notify chain (no notifier registered here, so only the
    // policy trampoline body is exercised).
    unsafe { ove::ffi::ove_pm_idle_process(); }

    ove::pm::clear_policy().unwrap();
    POLICY_CELL.shutdown();
    ove::pm::deinit();
}

fn test_pm_policy_trampoline_uninitialized_cell() {
    fresh_init();
    POLICY_CELL.shutdown(); // cell intentionally uninitialized
    ove::pm::set_policy(&POLICY_HANDLER).unwrap();

    // Trampoline's `try_get` returns None → early-return `current`.
    unsafe { ove::ffi::ove_pm_idle_process(); }

    ove::pm::clear_policy().unwrap();
    ove::pm::deinit();
}

fn test_pm_notify_trampoline_fires() {
    fresh_init();
    POLICY_CELL.shutdown();
    POLICY_CELL.init(PolicyState { forced: State::Idle });
    NOTIFY_CELL.shutdown();
    NOTIFY_CELL.init(NotifyState { _marker: 0 });

    ove::pm::set_policy(&POLICY_HANDLER).unwrap();
    ove::pm::notify_register(&NOTIFY_HANDLER).unwrap();

    // Single idle_process drives PRE_SLEEP (event=0) and POST_WAKE (event=1),
    // each with valid from/to mappings — hits both match arms in the
    // event-switch and both valid-state arms in `map`.
    unsafe { ove::ffi::ove_pm_idle_process(); }

    ove::pm::notify_unregister(&NOTIFY_HANDLER).unwrap();
    ove::pm::clear_policy().unwrap();
    NOTIFY_CELL.shutdown();
    POLICY_CELL.shutdown();
    ove::pm::deinit();
}

fn test_pm_notify_trampoline_uninitialized_cell() {
    fresh_init();
    POLICY_CELL.shutdown();
    POLICY_CELL.init(PolicyState { forced: State::Idle });
    NOTIFY_CELL.shutdown(); // cell intentionally uninitialized

    ove::pm::set_policy(&POLICY_HANDLER).unwrap();
    ove::pm::notify_register(&NOTIFY_HANDLER).unwrap();
    unsafe { ove::ffi::ove_pm_idle_process(); }

    ove::pm::notify_unregister(&NOTIFY_HANDLER).unwrap();
    ove::pm::clear_policy().unwrap();
    POLICY_CELL.shutdown();
    ove::pm::deinit();
}

fn test_pm_domain_refcount_without_init_errors() {
    ove::pm::deinit();
    // Backend returns INVALID_PARAM when uninitialised — exercises the
    // `rc < 0` error path in `domain_get_refcount`.
    assert!(ove::pm::domain_get_refcount(Domain::Audio).is_err());
}

/* ── Enum coverage sanity checks ────────────────────────────────────── */

fn test_pm_enum_values() {
    // Exercise the repr(u32) discriminants so Debug/Clone/Copy/Eq are linked.
    assert_eq!(State::Active as u32, 0);
    assert_eq!(State::DeepSleep as u32, 3);
    assert_eq!(WakeType::Gpio as u32, 0);
    assert_eq!(WakeType::Rtc as u32, 3);
    assert_eq!(Domain::Radio as u32, 0);
    assert_eq!(Domain::User1 as u32, 7);
    assert_eq!(Event::PreSleep as u32, 0);
    assert_eq!(Event::PostWake as u32, 1);

    let s = State::Idle;
    let t = s;
    assert_eq!(s, t);
    assert_ne!(State::Active, State::Idle);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "PM",
        &[
            test_entry!(test_pm_init_deinit),
            test_entry!(test_pm_deinit_idempotent),
            test_entry!(test_pm_double_init_fails),
            test_entry!(test_pm_get_state_after_init),
            test_entry!(test_pm_set_state_transitions),
            test_entry!(test_pm_set_state_same_is_ok),
            test_entry!(test_pm_set_state_without_init_errors),
            test_entry!(test_pm_activity),
            test_entry!(test_pm_wake_register_gpio),
            test_entry!(test_pm_wake_register_timer),
            test_entry!(test_pm_wake_register_uart),
            test_entry!(test_pm_wake_unregister_unknown_errors),
            test_entry!(test_pm_wake_register_table_full),
            test_entry!(test_pm_domain_request_release),
            test_entry!(test_pm_domain_release_underflow_errors),
            test_entry!(test_pm_domain_all_variants),
            test_entry!(test_pm_clear_policy),
            test_entry!(test_pm_set_policy_typed),
            test_entry!(test_pm_set_policy_raw),
            test_entry!(test_pm_notify_register_unregister),
            test_entry!(test_pm_notify_unregister_unknown_errors),
            test_entry!(test_pm_notify_register_raw),
            test_entry!(test_pm_policy_trampoline_fires),
            test_entry!(test_pm_policy_trampoline_uninitialized_cell),
            test_entry!(test_pm_notify_trampoline_fires),
            test_entry!(test_pm_notify_trampoline_uninitialized_cell),
            test_entry!(test_pm_domain_refcount_without_init_errors),
            test_entry!(test_pm_get_stats),
            test_entry!(test_pm_get_stats_without_init_errors),
            test_entry!(test_pm_reset_stats),
            test_entry!(test_pm_budget),
            test_entry!(test_pm_budget_without_init_errors),
            test_entry!(test_pm_enum_values),
        ],
    )
}
