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
//!   - Runtime power statistics reporting

#![cfg_attr(not(feature = "std"), no_std)]

use ove::{Priority, Thread};

#[cfg(has_pm)]
use ove::pm;

// ---------------------------------------------------------------------------
// Simulated battery level
// ---------------------------------------------------------------------------

static BATTERY_PCT: core::sync::atomic::AtomicI32 =
    core::sync::atomic::AtomicI32::new(85);

// ---------------------------------------------------------------------------
// Sensor thread: periodic read with domain management
// ---------------------------------------------------------------------------

fn sensor_entry() {
    let mut reading: u32 = 0;

    ove::log_inf!("sensor: started");

    loop {
        #[cfg(has_pm)]
        {
            let _ = pm::domain_request(pm::Domain::Sensor);
            pm::activity();
        }

        Thread::sleep_ms(50);
        reading = reading.wrapping_add(17);
        ove::log_inf!("sensor: reading = {}", reading % 1000);

        #[cfg(has_pm)]
        {
            let _ = pm::domain_release(pm::Domain::Sensor);
        }

        Thread::sleep_ms(5000);
    }
}

// ---------------------------------------------------------------------------
// Monitor thread: print power statistics
// ---------------------------------------------------------------------------

fn monitor_entry() {
    ove::log_inf!("monitor: started");

    loop {
        Thread::sleep_ms(10000);

        #[cfg(has_pm)]
        {
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
                    "  active%: {}.{:02}%",
                    stats.active_pct_x100 / 100,
                    stats.active_pct_x100 % 100
                );
            }
        }

        let batt = BATTERY_PCT.load(core::sync::atomic::Ordering::Relaxed);
        if batt > 5 {
            BATTERY_PCT.store(batt - 5, core::sync::atomic::Ordering::Relaxed);
        }
        ove::log_inf!("battery: {}%", batt);
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log_inf!("pm example (Rust): init");

    #[cfg(has_pm)]
    {
        let cfg = pm::Cfg {
            idle_threshold_ms: 50,
            standby_threshold_ms: 5000,
            deep_sleep_threshold_ms: 30000,
        };
        if let Err(e) = pm::init(&cfg) {
            ove::log_err!("PM init failed: {:?}", e);
            return;
        }

        // Register wake sources
        let _ = pm::wake_register_gpio(0, 13, 0x02); // falling edge
        let _ = pm::wake_register_uart(0);

        // Set power budget target: 60% low-power
        let _ = pm::set_budget(6000);
    }

    // Create threads
    let _sensor = ove::thread!("sensor", sensor_entry, Priority::Normal, 4096);
    let _monitor = ove::thread!("monitor", monitor_entry, Priority::Low, 4096);

    ove::log_inf!(
        "pm example (Rust): ready (battery={}%)",
        BATTERY_PCT.load(core::sync::atomic::Ordering::Relaxed)
    );

    ove::run();

    #[cfg(has_pm)]
    pm::deinit();

    ove::log_inf!("pm example (Rust): shutdown");
}

ove::main!(app_main);
