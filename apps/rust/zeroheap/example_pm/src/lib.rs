// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust Power Management Example — zero-heap mode.
//!
//! Threads are spawned via the `ove::thread!` macro, which expands to
//! function-scope `static mut <storage>` and `static mut <stack>` plus
//! the corresponding `Thread::from_static` call.  Shared state lives in
//! `ove::shared!` cells.  No `Box`, no `Arc`, no closure boxing.

#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicI32, Ordering};

use ove::pm::{self, Event, NotifyHandler, PolicyCtx, PolicyHandler, State};
use ove::{Priority, Thread};

/// Battery level in whole percent; the policy reads it, the monitor
/// thread drains it.
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

fn battery_policy(batt: &Battery, ctx: PolicyCtx) -> State {
    if batt.get() < 15 {
        return if ctx.idle_ms > 5 {
            State::DeepSleep
        } else {
            State::Standby
        };
    }
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

fn pm_notify(_batt: &Battery, event: Event, from: State, to: State) {
    match event {
        Event::PreSleep => ove::log_inf!("pm: preparing sleep {} -> {}", from as i32, to as i32),
        Event::PostWake => ove::log_inf!("pm: woke {} -> {}", from as i32, to as i32),
    }
}

static POLICY: PolicyHandler<Battery> = PolicyHandler::new(&BATTERY, battery_policy);
static NOTIFY: NotifyHandler<Battery> = NotifyHandler::new(&BATTERY, pm_notify);

fn sensor_entry() {
    ove::log_inf!("sensor: started");
    let mut reading: u32 = 0;
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

fn monitor_entry() {
    ove::log_inf!("monitor: started");
    loop {
        Thread::sleep_ms(10000);

        if let Ok(stats) = pm::get_stats() {
            ove::log_inf!("=== Power Stats ===");
            ove::log_inf!(
                "  active:  {} us ({} transitions)",
                stats.time_in_state_us[0],
                stats.transition_count[0]
            );
            ove::log_inf!(
                "  idle:    {} us ({} transitions)",
                stats.time_in_state_us[1],
                stats.transition_count[1]
            );
            ove::log_inf!(
                "  standby: {} us ({} transitions)",
                stats.time_in_state_us[2],
                stats.transition_count[2]
            );
            ove::log_inf!(
                "  deep:    {} us ({} transitions)",
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

fn app_main() {
    ove::log_inf!("pm example (zero-heap mode): init");

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

    pm::wake_register_gpio(0, 13, 0x02).ok();
    pm::wake_register_uart(0).ok();
    pm::notify_register(&NOTIFY).ok();
    pm::set_policy(&POLICY).ok();
    pm::set_budget(6000).ok();

    ove::thread!("sensor", sensor_entry, Priority::Normal, 4096).detach();
    ove::thread!("monitor", monitor_entry, Priority::Low, 4096).detach();

    ove::log_inf!(
        "pm example (zero-heap mode): ready (battery={}%)",
        BATTERY.get().get()
    );

    ove::run();

    ove::log_inf!("pm example (zero-heap mode): shutdown");
}

ove::main!(app_main);
