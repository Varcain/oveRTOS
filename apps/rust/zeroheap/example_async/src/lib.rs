// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust async demo — zero-heap mode.
//!
//! Mirror of `apps/rust/heap/example_async/` running with
//! `CONFIG_OVE_ZERO_HEAP=y` and no `alloc` feature on the `ove` crate.
//!
//! Differences from the heap variant:
//!
//!   - No `ove-allocator` dependency. The `#[ove::main]` macro detects
//!     `cfg(zero_heap)` and uses a function-scope `static mut
//!     MaybeUninit<Executor>` slot instead of `Box::leak`.
//!   - The async time driver uses a `static mut ove_timer_storage_t`
//!     slot for the global alarm timer (initialised by
//!     `ove_timer_init_ns`), not `ove_timer_create_ns`.
//!   - Tasks own their state on the stack / in `static`s. The
//!     `embassy_sync::Channel` is a `static` with a fixed-capacity
//!     internal buffer — no heap involved on either side.
//!   - `#[embassy_executor::task]` task pools are statically sized via
//!     the upstream `embassy_executor::task` macro's default pool size.
//!
//! What's exercised:
//!   1. `#[ove::main] async fn(spawner: Spawner)` macro
//!   2. Embassy executor on an `ove::Thread`, started via `ove::run()`
//!   3. `embassy_time::Timer::after_millis(250)` — ove_timer-backed
//!   4. `embassy_sync::Channel` ping-pong between two tasks

#![cfg_attr(not(feature = "std"), no_std)]

use embassy_executor::Spawner;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::{Duration, Timer};

static CHANNEL: Channel<CriticalSectionRawMutex, u32, 4> = Channel::new();

#[embassy_executor::task]
async fn blinker() {
    let mut counter: u32 = 0;
    loop {
        log::info!("tick {counter}");
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
    ove::printk!("oveRTOS Rust async demo (zero-heap) starting\n");
    let _ = ove::log::try_init();

    spawner.must_spawn(blinker());
    spawner.must_spawn(consumer());
}
