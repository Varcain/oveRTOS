// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Power management framework for oveRTOS.
//!
//! Provides a unified interface for sleep state management, peripheral power
//! domains, wake source registration, pluggable power policies, and runtime
//! power statistics.
//!
//! The PM subsystem is a singleton — there is one system-wide power state.
//! Initialise with [`init`] and tear down with [`deinit`].

use crate::bindings;
use crate::error::{Error, Result};

// ── Enumerations ────────────────────────────────────────────────────────

/// System power states, ordered by increasing sleep depth.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum State {
    /// Full speed, all clocks running.
    Active = 0,
    /// Light sleep, fast wakeup, peripherals on.
    Idle = 1,
    /// Deep idle, some peripherals off.
    Standby = 2,
    /// Lowest power, RAM retained, slow wakeup.
    DeepSleep = 3,
}

/// Wake source types.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum WakeType {
    /// GPIO pin edge.
    Gpio = 0,
    /// Timer expiry.
    Timer = 1,
    /// UART RX activity.
    Uart = 2,
    /// RTC alarm.
    Rtc = 3,
}

/// Peripheral power domain identifiers.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Domain {
    Radio = 0,
    Sensor = 1,
    Display = 2,
    Audio = 3,
    Storage = 4,
    Comms = 5,
    User0 = 6,
    User1 = 7,
}

/// Transition event type for notification callbacks.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Event {
    /// About to enter low-power state.
    PreSleep = 0,
    /// Just woke from low-power state.
    PostWake = 1,
}

// ── Configuration ───────────────────────────────────────────────────────

/// PM subsystem configuration.
#[derive(Debug, Copy, Clone)]
pub struct Cfg {
    /// Idle ms before ACTIVE to IDLE transition.
    pub idle_threshold_ms: u32,
    /// Idle ms before IDLE to STANDBY transition.
    pub standby_threshold_ms: u32,
    /// Idle ms before DEEP_SLEEP transition.
    pub deep_sleep_threshold_ms: u32,
}

/// Runtime power statistics.
#[derive(Debug, Copy, Clone)]
pub struct Stats {
    /// Cumulative microseconds in each state.
    pub time_in_state_us: [u64; 4],
    /// Number of entries per state.
    pub transition_count: [u32; 4],
    /// Total tracked runtime in microseconds.
    pub total_runtime_us: u64,
    /// Active percentage in hundredths (0..10000).
    pub active_pct_x100: u32,
}

// ── Lifecycle ───────────────────────────────────────────────────────────

/// Initialise the PM subsystem.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `cfg` values are invalid.
pub fn init(cfg: &Cfg) -> Result<()> {
    let c_cfg = bindings::ove_pm_cfg {
        idle_threshold_ms: cfg.idle_threshold_ms,
        standby_threshold_ms: cfg.standby_threshold_ms,
        deep_sleep_threshold_ms: cfg.deep_sleep_threshold_ms,
    };
    let rc = unsafe { bindings::ove_pm_init(&c_cfg) };
    Error::from_code(rc)
}

/// Tear down the PM subsystem and release resources.
pub fn deinit() {
    unsafe { bindings::ove_pm_deinit() }
}

// ── State machine ───────────────────────────────────────────────────────

/// Request an explicit power state transition.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `state` is out of range.
pub fn set_state(state: State) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_set_state(state as bindings::ove_pm_state_t) };
    Error::from_code(rc)
}

/// Query the current power state.
pub fn get_state() -> State {
    let raw = unsafe { bindings::ove_pm_get_state() };
    match raw {
        0 => State::Active,
        1 => State::Idle,
        2 => State::Standby,
        3 => State::DeepSleep,
        _ => State::Active,
    }
}

/// Report system activity (ISR-safe). Resets the idle timer.
pub fn activity() {
    unsafe { bindings::ove_pm_activity() }
}

// ── Wake sources ────────────────────────────────────────────────────────

/// Register a GPIO wake source.
///
/// # Errors
/// Returns [`Error::NoMemory`] if the wake source table is full.
pub fn wake_register_gpio(port: u32, pin: u32, edge: u32) -> Result<()> {
    let mut src: bindings::ove_pm_wake_src = unsafe { core::mem::zeroed() };
    src.type_ = WakeType::Gpio as bindings::ove_pm_wake_type_t;
    unsafe {
        src.__bindgen_anon_1.gpio.port = port;
        src.__bindgen_anon_1.gpio.pin = pin;
        src.__bindgen_anon_1.gpio.edge = edge as bindings::ove_gpio_irq_mode_t;
    }
    let rc = unsafe { bindings::ove_pm_wake_register(&src) };
    Error::from_code(rc)
}

/// Register a timer wake source.
///
/// # Errors
/// Returns [`Error::NoMemory`] if the wake source table is full.
pub fn wake_register_timer(timeout_ms: u32) -> Result<()> {
    let mut src: bindings::ove_pm_wake_src = unsafe { core::mem::zeroed() };
    src.type_ = WakeType::Timer as bindings::ove_pm_wake_type_t;
    unsafe { src.__bindgen_anon_1.timer.timeout_ms = timeout_ms };
    let rc = unsafe { bindings::ove_pm_wake_register(&src) };
    Error::from_code(rc)
}

/// Register a UART wake source.
///
/// # Errors
/// Returns [`Error::NoMemory`] if the wake source table is full.
pub fn wake_register_uart(instance: u32) -> Result<()> {
    let mut src: bindings::ove_pm_wake_src = unsafe { core::mem::zeroed() };
    src.type_ = WakeType::Uart as bindings::ove_pm_wake_type_t;
    unsafe { src.__bindgen_anon_1.uart.instance = instance };
    let rc = unsafe { bindings::ove_pm_wake_register(&src) };
    Error::from_code(rc)
}

/// Unregister a GPIO wake source.
///
/// # Errors
/// Returns [`Error::NotRegistered`] if the wake source was not found.
pub fn wake_unregister_gpio(port: u32, pin: u32) -> Result<()> {
    let mut src: bindings::ove_pm_wake_src = unsafe { core::mem::zeroed() };
    src.type_ = WakeType::Gpio as bindings::ove_pm_wake_type_t;
    unsafe {
        src.__bindgen_anon_1.gpio.port = port;
        src.__bindgen_anon_1.gpio.pin = pin;
    }
    let rc = unsafe { bindings::ove_pm_wake_unregister(&src) };
    Error::from_code(rc)
}

// ── Peripheral power domains ────────────────────────────────────────────

/// Increment the reference count for a peripheral power domain.
///
/// On the first request (0 to 1), the domain hardware is powered on.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if `domain` is out of range.
pub fn domain_request(domain: Domain) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_domain_request(domain as bindings::ove_pm_domain_t) };
    Error::from_code(rc)
}

/// Decrement the reference count for a peripheral power domain.
///
/// When the count reaches zero, the domain hardware is powered off.
///
/// # Errors
/// Returns [`Error::InvalidParam`] on underflow.
pub fn domain_release(domain: Domain) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_domain_release(domain as bindings::ove_pm_domain_t) };
    Error::from_code(rc)
}

/// Query the current reference count for a domain.
///
/// # Errors
/// Returns a negative error code if `domain` is invalid.
pub fn domain_get_refcount(domain: Domain) -> Result<i32> {
    let rc = unsafe { bindings::ove_pm_domain_get_refcount(domain as bindings::ove_pm_domain_t) };
    if rc < 0 {
        Error::from_code(rc)?;
    }
    Ok(rc)
}

// ── Policy ──────────────────────────────────────────────────────────────

/// Raw-pointer variant of [`set_policy`].
///
/// # Safety
/// The callback and `user_data` must remain valid for the lifetime of the
/// registration. Prefer the safe [`PolicyHandler`]-based [`set_policy`].
pub unsafe fn set_policy_raw(
    policy: bindings::ove_pm_policy_fn,
    user_data: *mut core::ffi::c_void,
) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_set_policy(policy, user_data) };
    Error::from_code(rc)
}

/// Per-tick context passed to a [`PolicyHandler`].
#[derive(Debug, Copy, Clone)]
pub struct PolicyCtx {
    pub current: State,
    pub idle_ms: u32,
    pub next_timeout_ms: u32,
}

/// A registered power-policy handler bound to a static state cell.
///
/// Construct with [`PolicyHandler::new`] and register with [`set_policy`].
/// The handler must be `'static` (typically declared at module scope with
/// [`crate::shared!`] + this type).
pub struct PolicyHandler<T: Send + Sync + 'static> {
    cell: &'static crate::StaticCell<T>,
    user: fn(&T, PolicyCtx) -> State,
}

impl<T: Send + Sync + 'static> PolicyHandler<T> {
    pub const fn new(
        cell: &'static crate::StaticCell<T>,
        user: fn(&T, PolicyCtx) -> State,
    ) -> Self {
        Self { cell, user }
    }
}

unsafe impl<T: Send + Sync + 'static> Sync for PolicyHandler<T> {}

unsafe extern "C" fn policy_trampoline<T: Send + Sync + 'static>(
    current: bindings::ove_pm_state_t,
    idle_ms: u32,
    next_timeout_ms: u32,
    user_data: *mut core::ffi::c_void,
) -> bindings::ove_pm_state_t {
    if user_data.is_null() {
        return current;
    }
    // SAFETY: `user_data` was set by `set_policy` from a `&'static PolicyHandler<T>`.
    let h = unsafe { &*(user_data as *const PolicyHandler<T>) };
    let Some(state) = h.cell.try_get() else { return current; };
    let ctx = PolicyCtx {
        current: match current {
            0 => State::Active,
            1 => State::Idle,
            2 => State::Standby,
            3 => State::DeepSleep,
            _ => State::Active,
        },
        idle_ms,
        next_timeout_ms,
    };
    (h.user)(state, ctx) as bindings::ove_pm_state_t
}

/// Register a typed-context power policy handler.
pub fn set_policy<T: Send + Sync + 'static>(
    handler: &'static PolicyHandler<T>,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_pm_set_policy(
            Some(policy_trampoline::<T>),
            handler as *const _ as *mut core::ffi::c_void,
        )
    };
    Error::from_code(rc)
}

/// Restore the default threshold-based power policy.
pub fn clear_policy() -> Result<()> {
    let rc = unsafe { bindings::ove_pm_set_policy(None, core::ptr::null_mut()) };
    Error::from_code(rc)
}

// ── Notifications ───────────────────────────────────────────────────────

/// Raw-pointer variant of [`notify_register`].
///
/// # Safety
/// The callback and `user_data` must remain valid until unregistered.
pub unsafe fn notify_register_raw(
    cb: bindings::ove_pm_notify_fn,
    user_data: *mut core::ffi::c_void,
) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_notify_register(cb, user_data) };
    Error::from_code(rc)
}

/// Raw-pointer variant of [`notify_unregister`].
///
/// # Safety
/// Must match a previously registered (cb, user_data) pair.
pub unsafe fn notify_unregister_raw(
    cb: bindings::ove_pm_notify_fn,
    user_data: *mut core::ffi::c_void,
) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_notify_unregister(cb, user_data) };
    Error::from_code(rc)
}

/// A registered power transition notification handler.
///
/// Bound to a static state cell and a safe `fn(&T, Event, State, State)` callback.
pub struct NotifyHandler<T: Send + Sync + 'static> {
    cell: &'static crate::StaticCell<T>,
    user: fn(&T, Event, State, State),
}

impl<T: Send + Sync + 'static> NotifyHandler<T> {
    pub const fn new(
        cell: &'static crate::StaticCell<T>,
        user: fn(&T, Event, State, State),
    ) -> Self {
        Self { cell, user }
    }
}

unsafe impl<T: Send + Sync + 'static> Sync for NotifyHandler<T> {}

unsafe extern "C" fn notify_trampoline<T: Send + Sync + 'static>(
    event: bindings::ove_pm_event_t,
    from_state: bindings::ove_pm_state_t,
    to_state: bindings::ove_pm_state_t,
    user_data: *mut core::ffi::c_void,
) {
    if user_data.is_null() {
        return;
    }
    // SAFETY: `user_data` was set by `notify_register` from a `&'static NotifyHandler<T>`.
    let h = unsafe { &*(user_data as *const NotifyHandler<T>) };
    let Some(state) = h.cell.try_get() else { return; };
    let ev = match event {
        0 => Event::PreSleep,
        1 => Event::PostWake,
        _ => return,
    };
    let map = |s: bindings::ove_pm_state_t| match s {
        0 => State::Active,
        1 => State::Idle,
        2 => State::Standby,
        3 => State::DeepSleep,
        _ => State::Active,
    };
    (h.user)(state, ev, map(from_state), map(to_state));
}

/// Register a typed-context transition notification handler.
///
/// # Errors
/// Returns [`Error::NoMemory`] if the notifier table is full.
pub fn notify_register<T: Send + Sync + 'static>(
    handler: &'static NotifyHandler<T>,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_pm_notify_register(
            Some(notify_trampoline::<T>),
            handler as *const _ as *mut core::ffi::c_void,
        )
    };
    Error::from_code(rc)
}

/// Unregister a previously registered notification handler.
///
/// # Errors
/// Returns [`Error::NotRegistered`] if the handler was not found.
pub fn notify_unregister<T: Send + Sync + 'static>(
    handler: &'static NotifyHandler<T>,
) -> Result<()> {
    let rc = unsafe {
        bindings::ove_pm_notify_unregister(
            Some(notify_trampoline::<T>),
            handler as *const _ as *mut core::ffi::c_void,
        )
    };
    Error::from_code(rc)
}

// ── Statistics ──────────────────────────────────────────────────────────

/// Query accumulated power statistics.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if the PM subsystem is not initialised.
pub fn get_stats() -> Result<Stats> {
    let mut raw: bindings::ove_pm_stats = unsafe { core::mem::zeroed() };
    let rc = unsafe { bindings::ove_pm_get_stats(&mut raw) };
    Error::from_code(rc)?;
    Ok(Stats {
        time_in_state_us: raw.time_in_state_us,
        transition_count: raw.transition_count,
        total_runtime_us: raw.total_runtime_us,
        active_pct_x100: raw.active_pct_x100,
    })
}

/// Reset all accumulated power statistics to zero.
pub fn reset_stats() {
    unsafe { bindings::ove_pm_reset_stats() }
}

// ── Power budget ────────────────────────────────────────────────────────

/// Set a target percentage of time in low-power states.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if the PM subsystem is not initialised.
pub fn set_budget(target_low_power_pct_x100: u32) -> Result<()> {
    let rc = unsafe { bindings::ove_pm_set_budget(target_low_power_pct_x100) };
    Error::from_code(rc)
}

/// Query actual low-power percentage vs. budget target.
///
/// Returns the actual low-power percentage in hundredths (0..10000).
///
/// # Errors
/// Returns [`Error::InvalidParam`] if the PM subsystem is not initialised.
pub fn get_budget_status() -> Result<u32> {
    let mut actual: u32 = 0;
    let rc = unsafe { bindings::ove_pm_get_budget_status(&mut actual) };
    Error::from_code(rc)?;
    Ok(actual)
}
