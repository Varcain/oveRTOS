// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust Power Management Example — heap mode.
//!
//! Threads are spawned via `Thread::spawn_with(...)` taking `FnOnce`
//! closures.  PM policy / notify handlers necessarily go through
//! `&'static InitCell<T>` because the C ABI takes a raw pointer and
//! the binding pins it for the program lifetime — there is no heap
//! analogue.
//!
//! Pair with apps/rust/zeroheap/example_pm/ which uses the same
//! `InitCell` plus `ove::thread!` macros against caller-supplied
//! storage.

#![cfg_attr(not(feature = "std"), no_std)]

use ove_allocator as _;

use core::sync::atomic::{AtomicI32, Ordering};

use ove::pm::{self, Event, NotifyHandler, PolicyCtx, PolicyHandler, State};
use ove::{Priority, InitCell, Thread};

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

static BATTERY: InitCell<Battery> = InitCell::new();

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
        Event::PreSleep => log::info!("pm: preparing sleep {} -> {}", from as i32, to as i32),
        Event::PostWake => log::info!("pm: woke {} -> {}", from as i32, to as i32),
    }
}

static POLICY: PolicyHandler<Battery> = PolicyHandler::new(&BATTERY, battery_policy);
static NOTIFY: NotifyHandler<Battery> = NotifyHandler::new(&BATTERY, pm_notify);

fn log_stats(stats: &pm::Stats) {
    log::info!("=== Power Stats ===");
    log::info!(
        "  active:  {} us ({} transitions)",
        stats.time_in_state_us[0],
        stats.transition_count[0]
    );
    log::info!(
        "  idle:    {} us ({} transitions)",
        stats.time_in_state_us[1],
        stats.transition_count[1]
    );
    log::info!(
        "  standby: {} us ({} transitions)",
        stats.time_in_state_us[2],
        stats.transition_count[2]
    );
    log::info!(
        "  deep:    {} us ({} transitions)",
        stats.time_in_state_us[3],
        stats.transition_count[3]
    );
    log::info!(
        "  active%: {}.{:02}%",
        stats.active_pct_x100 / 100,
        stats.active_pct_x100 % 100
    );
}

fn app_main() {
    ove::log::try_init();
    log::info!("pm example (heap mode): init");

    BATTERY.init(Battery::new(85));

    let cfg = pm::Cfg {
        idle_threshold_ms: 50,
        standby_threshold_ms: 5000,
        deep_sleep_threshold_ms: 30000,
    };
    if let Err(e) = pm::init(&cfg) {
        log::error!("PM init failed: {:?}", e);
        return;
    }

    pm::wake_register_gpio(0, 13, 0x02).ok();
    pm::wake_register_uart(0).ok();
    pm::notify_register(&NOTIFY).ok();
    pm::set_policy(&POLICY).ok();
    pm::set_budget(6000).ok();

    let _sensor = Thread::builder().name(c"sensor").priority(Priority::Normal).stack_size(4096).spawn(|_tok| {
        log::info!("sensor: started");
        let mut reading: u32 = 0;
        loop {
            pm::domain_request(pm::Domain::Sensor).ok();
            pm::activity();
            Thread::sleep_ms(50);
            reading = reading.wrapping_add(17);
            log::info!("sensor: reading = {}", reading % 1000);
            pm::domain_release(pm::Domain::Sensor).ok();
            Thread::sleep_ms(5000);
        }
    })
    .expect("sensor spawn");

    let _monitor = Thread::builder().name(c"monitor").priority(Priority::Low).stack_size(4096).spawn(|_tok| {
        log::info!("monitor: started");
        loop {
            Thread::sleep_ms(10000);
            if let Ok(stats) = pm::get_stats() {
                log_stats(&stats);
            }
            if let Some(batt) = BATTERY.try_get() {
                batt.drain(5);
                log::info!("battery: {}%", batt.get());
            }
        }
    })
    .expect("monitor spawn");

    log::info!(
        "pm example (heap mode): ready (battery={}%)",
        BATTERY.get().get()
    );

    core::mem::forget(_sensor);
    core::mem::forget(_monitor);

    ove::run();

    log::info!("pm example (heap mode): shutdown");
}

ove::main!(app_main);
