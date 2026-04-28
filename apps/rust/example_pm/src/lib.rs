// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust Power Management Example
//!
//! Demonstrates the `ove::pm` module:
//!   - Sleep state machine with auto-idle
//!   - Peripheral power domain reference counting
//!   - Wake source registration (GPIO + UART)
//!   - Custom battery-aware power policy
//!   - Transition notification callbacks
//!   - Runtime power statistics reporting

#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicI32, Ordering};

use ove::pm::{self, Event, NotifyHandler, PolicyCtx, PolicyHandler, State};
use ove::{Priority, Thread};

// ---------------------------------------------------------------------------
// Shared state for policy + notify handlers
// ---------------------------------------------------------------------------

/// Battery level in whole percent; policy reads, monitor updates.
struct Battery(AtomicI32);

impl Battery {
    const fn new(pct: i32) -> Self {
        Self(AtomicI32::new(pct))
    }
    fn get(&self) -> i32 {
        self.0.load(Ordering::Relaxed)
    }
    fn drain(&self, by: i32) {
        let cur = self.get();
        if cur > by {
            self.0.store(cur - by, Ordering::Relaxed);
        }
    }
}

ove::shared!(BATTERY: Battery);

// ---------------------------------------------------------------------------
// Battery-aware power policy (mirrors C example)
// ---------------------------------------------------------------------------

fn battery_policy(batt: &Battery, ctx: PolicyCtx) -> State {
    // Aggressive sleep when battery is critically low
    if batt.get() < 15 {
        return if ctx.idle_ms > 5 {
            State::DeepSleep
        } else {
            State::Standby
        };
    }
    // Normal thresholds
    if ctx.idle_ms < 10 {
        State::Active
    } else if ctx.idle_ms < 1000 {
        State::Idle
    } else if ctx.idle_ms < 10000 {
        State::Standby
    } else {
        State::DeepSleep
    }
}

static POLICY: PolicyHandler<Battery> = PolicyHandler::new(&BATTERY, battery_policy);

// ---------------------------------------------------------------------------
// PM transition notifier (mirrors C example)
// ---------------------------------------------------------------------------

fn pm_notify(_batt: &Battery, event: Event, from: State, to: State) {
    match event {
        Event::PreSleep => ove::log_inf!("pm: preparing sleep {:?} -> {:?}", from, to),
        Event::PostWake => ove::log_inf!("pm: woke {:?} -> {:?}", from, to),
    }
}

static NOTIFY: NotifyHandler<Battery> = NotifyHandler::new(&BATTERY, pm_notify);

// ---------------------------------------------------------------------------
// Sensor thread: periodic read with domain management
// ---------------------------------------------------------------------------

fn sensor_entry() {
    let mut reading: u32 = 0;

    ove::log_inf!("sensor: started");

    loop {
        pm::domain_request(pm::Domain::Sensor).ok();
        pm::activity();

        Thread::sleep_ms(50);
        reading = reading.wrapping_add(17);
        ove::log_inf!("sensor: reading = {}", reading % 1000);

        pm::domain_release(pm::Domain::Sensor).ok();

        Thread::sleep_ms(5000);
    }
}

// ---------------------------------------------------------------------------
// Monitor thread: print power statistics and drain battery
// ---------------------------------------------------------------------------

fn monitor_entry() {
    ove::log_inf!("monitor: started");

    loop {
        Thread::sleep_ms(10000);

        if let Ok(stats) = pm::get_stats() {
            ove::log_inf!("=== Power Stats ===");
            ove::log_inf!(
                "  active:  {} us ({} trans)",
                stats.time_in_state_us[0],
                stats.transition_count[0]
            );
            ove::log_inf!(
                "  idle:    {} us ({} trans)",
                stats.time_in_state_us[1],
                stats.transition_count[1]
            );
            ove::log_inf!(
                "  standby: {} us ({} trans)",
                stats.time_in_state_us[2],
                stats.transition_count[2]
            );
            ove::log_inf!(
                "  deep:    {} us ({} trans)",
                stats.time_in_state_us[3],
                stats.transition_count[3]
            );
            ove::log_inf!(
                "  active%: {}.{:02}%",
                stats.active_pct_x100 / 100,
                stats.active_pct_x100 % 100
            );
        }

        if let Some(batt) = BATTERY.try_get() {
            batt.drain(5);
            ove::log_inf!("battery: {}%", batt.get());
        }
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log_inf!("pm example (Rust): init");

    BATTERY.init(Battery::new(85));

    let cfg = pm::Cfg {
        idle_threshold_ms: 50,
        standby_threshold_ms: 5000,
        deep_sleep_threshold_ms: 30000,
    };
    if let Err(e) = pm::init(&cfg) {
        ove::log_err!("PM init failed: {:?}", e);
        return;
    }

    // Register wake sources (GPIO button + UART RX)
    pm::wake_register_gpio(0, 13, 0x02).ok();
    pm::wake_register_uart(0).ok();

    // Register transition notifier + battery-aware policy
    pm::notify_register(&NOTIFY).ok();
    pm::set_policy(&POLICY).ok();

    // Set power budget target: 60% low-power
    pm::set_budget(6000).ok();

    let _sensor = ove::thread!("sensor", sensor_entry, Priority::Normal, 4096);
    let _monitor = ove::thread!("monitor", monitor_entry, Priority::Low, 4096);

    ove::log_inf!(
        "pm example (Rust): ready (battery={}%)",
        BATTERY.try_get().map(|b| b.get()).unwrap_or(0)
    );

    ove::run();

    pm::deinit();

    ove::log_inf!("pm example (Rust): shutdown");
}

ove::main!(app_main);
