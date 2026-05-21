// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust async demo — heap mode.
//!
//! Phase 1 vertical-slice demo for the Embassy integration. Spawns two
//! tasks:
//!
//!   - `blinker`: prints "tick N" every 250 ms via
//!     `embassy_time::Timer::after_millis(...).await`. Exercises the
//!     time driver (ove_time_get_us + ove_timer_init_ns alarm) and the
//!     executor's wake-on-deadline path.
//!   - `consumer`: receives messages from `blinker` over an
//!     `embassy_sync::Channel`. Exercises the pure-Rust async channel
//!     primitive (no C-side notify hook needed — both sides are Rust
//!     tasks on the same executor).
//!
//! Together they validate that:
//!   1. `#[ove::main] async fn(spawner: Spawner)` runs the executor.
//!   2. Multiple tasks can be spawned and make progress concurrently.
//!   3. Timers wake at approximately the requested cadence.
//!   4. Async channel send/recv works between tasks.

#![cfg_attr(not(feature = "std"), no_std)]

use ove_allocator as _;

use embassy_executor::Spawner;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::{Duration, Timer};

// A single async channel for blinker → consumer messages. Static so
// both tasks can hold references that satisfy the spawnable
// `'static` bound.
static CHANNEL: Channel<CriticalSectionRawMutex, u32, 4> = Channel::new();

#[embassy_executor::task]
async fn blinker() {
    let mut counter: u32 = 0;
    loop {
        log::info!("tick {counter}");
        // Best-effort send; drop the message if the channel is full so
        // the producer never blocks the timer cadence.
        let _ = CHANNEL.try_send(counter);
        counter = counter.wrapping_add(1);
        Timer::after(Duration::from_millis(250)).await;
    }
}

#[embassy_executor::task]
async fn consumer() {
    loop {
        let n = CHANNEL.receive().await;
        log::info!("consumer got {n}");
    }
}

#[ove::main]
async fn app_main(spawner: Spawner) {
    // Boot banner via direct console write — if `log::try_init` fails
    // (no console backend, log mutex unavailable, etc.) the message
    // still lands on the UART so the user isn't staring at a blank
    // serial port.
    ove::printk!("oveRTOS Rust async demo starting\n");
    let _ = ove::log::try_init();

    spawner.must_spawn(blinker());
    spawner.must_spawn(consumer());
    // Executor::run never returns. Once this fn returns, the run loop
    // continues polling and we never re-enter the user code.
}
