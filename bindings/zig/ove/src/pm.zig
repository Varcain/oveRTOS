// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Power management framework for oveRTOS.
//!
//! Provides a unified interface for sleep state management, peripheral power
//! domains, wake source registration, pluggable policies, and power statistics.
//!
//! The PM subsystem is a singleton — there is one system-wide power state.
//! Initialise with `init()` and tear down with `deinit()`.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

// ── Enumerations ────────────────────────────────────────────────────────

/// System power states (maps to `ove_pm_state_t`).
pub const State = c.ove_pm_state_t;

/// Wake source types (maps to `ove_pm_wake_type_t`).
pub const WakeType = c.ove_pm_wake_type_t;

/// Peripheral power domain identifiers (maps to `ove_pm_domain_t`).
pub const Domain = c.ove_pm_domain_t;

/// Transition event type (maps to `ove_pm_event_t`).
pub const Event = c.ove_pm_event_t;

/// Power statistics snapshot.
pub const Stats = struct {
    time_in_state_us: [4]u64,
    transition_count: [4]u32,
    total_runtime_us: u64,
    active_pct_x100: u32,
};

/// PM subsystem configuration.
pub const Cfg = struct {
    idle_threshold_ms: u32 = 10,
    standby_threshold_ms: u32 = 1000,
    deep_sleep_threshold_ms: u32 = 10000,
};

// ── Lifecycle ───────────────────────────────────────────────────────────

/// Initialise the PM subsystem.
pub fn init(cfg: Cfg) Error!void {
    const c_cfg = c.ove_pm_cfg{
        .idle_threshold_ms = cfg.idle_threshold_ms,
        .standby_threshold_ms = cfg.standby_threshold_ms,
        .deep_sleep_threshold_ms = cfg.deep_sleep_threshold_ms,
    };
    try err.fromCode(c.ove_pm_init(&c_cfg));
}

/// Tear down the PM subsystem.
pub fn deinit() void {
    c.ove_pm_deinit();
}

// ── State machine ───────────────────────────────────────────────────────

/// Request an explicit power state transition.
pub fn setState(state: State) Error!void {
    try err.fromCode(c.ove_pm_set_state(state));
}

/// Query the current power state.
pub fn getState() State {
    return c.ove_pm_get_state();
}

/// Report system activity (ISR-safe). Resets the idle timer.
pub fn activity() void {
    c.ove_pm_activity();
}

// ── Wake sources ────────────────────────────────────────────────────────

/// Register a GPIO wake source.
pub fn wakeRegisterGpio(port: u32, pin: u32, edge: u32) Error!void {
    var src: c.ove_pm_wake_src = std.mem.zeroes(c.ove_pm_wake_src);
    src.type = c.OVE_PM_WAKE_GPIO;
    src.unnamed_0.gpio.port = port;
    src.unnamed_0.gpio.pin = pin;
    src.unnamed_0.gpio.edge = edge;
    try err.fromCode(c.ove_pm_wake_register(&src));
}

/// Register a timer wake source.
pub fn wakeRegisterTimer(timeout_ms: u32) Error!void {
    var src: c.ove_pm_wake_src = std.mem.zeroes(c.ove_pm_wake_src);
    src.type = c.OVE_PM_WAKE_TIMER;
    src.unnamed_0.timer.timeout_ms = timeout_ms;
    try err.fromCode(c.ove_pm_wake_register(&src));
}

/// Register a UART wake source.
pub fn wakeRegisterUart(instance: u32) Error!void {
    var src: c.ove_pm_wake_src = std.mem.zeroes(c.ove_pm_wake_src);
    src.type = c.OVE_PM_WAKE_UART;
    src.unnamed_0.uart.instance = instance;
    try err.fromCode(c.ove_pm_wake_register(&src));
}

/// Unregister a GPIO wake source.
pub fn wakeUnregisterGpio(port: u32, pin: u32) Error!void {
    var src: c.ove_pm_wake_src = std.mem.zeroes(c.ove_pm_wake_src);
    src.type = c.OVE_PM_WAKE_GPIO;
    src.unnamed_0.gpio.port = port;
    src.unnamed_0.gpio.pin = pin;
    try err.fromCode(c.ove_pm_wake_unregister(&src));
}

// ── Peripheral power domains ────────────────────────────────────────────

/// Increment the reference count for a peripheral power domain.
pub fn domainRequest(domain: Domain) Error!void {
    try err.fromCode(c.ove_pm_domain_request(domain));
}

/// Decrement the reference count for a peripheral power domain.
pub fn domainRelease(domain: Domain) Error!void {
    try err.fromCode(c.ove_pm_domain_release(domain));
}

/// Query the current reference count for a domain.
pub fn domainGetRefcount(domain: Domain) Error!i32 {
    return err.fromCodeInt(c.ove_pm_domain_get_refcount(domain));
}

// ── Policy ──────────────────────────────────────────────────────────────

/// Register a custom power policy callback.
///
/// Pass `null` for `policy` to restore the default threshold-based policy.
pub fn setPolicy(
    policy: ?*const fn (State, u32, u32, ?*anyopaque) callconv(.c) State,
    user_data: ?*anyopaque,
) Error!void {
    try err.fromCode(c.ove_pm_set_policy(policy, user_data));
}

// ── Notifications ───────────────────────────────────────────────────────

/// Register a transition notification callback using a comptime function.
pub fn notifyRegister(
    comptime callback: fn (Event, State, State) void,
) Error!void {
    const Trampoline = struct {
        fn invoke(event: c.ove_pm_event_t, from: c.ove_pm_state_t, to: c.ove_pm_state_t, _: ?*anyopaque) callconv(.c) void {
            callback(event, from, to);
        }
    };
    try err.fromCode(c.ove_pm_notify_register(&Trampoline.invoke, null));
}

/// Register a raw C notification callback.
pub fn notifyRegisterRaw(
    cb: ?*const fn (c.ove_pm_event_t, c.ove_pm_state_t, c.ove_pm_state_t, ?*anyopaque) callconv(.c) void,
    user_data: ?*anyopaque,
) Error!void {
    try err.fromCode(c.ove_pm_notify_register(cb, user_data));
}

/// Unregister a raw C notification callback.
pub fn notifyUnregisterRaw(
    cb: ?*const fn (c.ove_pm_event_t, c.ove_pm_state_t, c.ove_pm_state_t, ?*anyopaque) callconv(.c) void,
    user_data: ?*anyopaque,
) Error!void {
    try err.fromCode(c.ove_pm_notify_unregister(cb, user_data));
}

// ── Statistics ──────────────────────────────────────────────────────────

/// Query accumulated power statistics.
pub fn getStats() Error!Stats {
    var raw: c.ove_pm_stats = std.mem.zeroes(c.ove_pm_stats);
    try err.fromCode(c.ove_pm_get_stats(&raw));
    return Stats{
        .time_in_state_us = raw.time_in_state_us,
        .transition_count = raw.transition_count,
        .total_runtime_us = raw.total_runtime_us,
        .active_pct_x100 = raw.active_pct_x100,
    };
}

/// Reset all accumulated power statistics to zero.
pub fn resetStats() void {
    c.ove_pm_reset_stats();
}

// ── Power budget ────────────────────────────────────────────────────────

/// Set a target percentage of time in low-power states.
pub fn setBudget(target_low_power_pct_x100: u32) Error!void {
    try err.fromCode(c.ove_pm_set_budget(target_low_power_pct_x100));
}

/// Query actual low-power percentage vs. budget target.
pub fn getBudgetStatus() Error!u32 {
    var actual: u32 = 0;
    try err.fromCode(c.ove_pm_get_budget_status(&actual));
    return actual;
}
