// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

#![cfg_attr(not(feature = "std"), no_std)]

use core::fmt::Write;
use core::sync::atomic::{AtomicU32, Ordering};
#[cfg(has_lvgl)]
use ove::lvgl::{self, Bar, Color, Label, Layout, Styleable};
use ove::{Error, FmtBuf, Priority, Queue, Thread, Timer, WAIT_FOREVER};

// ---------------------------------------------------------------------------
// Shared state (file-scope statics)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#[cfg(rtos_freertos)]
const APP_TITLE: &[u8] = b"oveRTOS(FreeRTOS) Rust Demo\0";
#[cfg(rtos_nuttx)]
const APP_TITLE: &[u8] = b"oveRTOS(NuttX) Rust Demo\0";
#[cfg(rtos_zephyr)]
const APP_TITLE: &[u8] = b"oveRTOS(Zephyr) Rust Demo\0";
#[cfg(rtos_posix)]
const APP_TITLE: &[u8] = b"oveRTOS(POSIX) Rust Demo\0";
#[cfg(not(any(rtos_freertos, rtos_nuttx, rtos_zephyr, rtos_posix)))]
const APP_TITLE: &[u8] = b"oveRTOS Rust Demo\0";

ove::shared!(QUEUE: Queue<u32, 8>);
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);
#[cfg(has_lvgl)]
ove::shared!(UI_TIMER: Timer);
#[cfg(has_lvgl)]
ove::shared!(COUNTER_LABEL: Label);
#[cfg(has_lvgl)]
ove::shared!(BAR: Bar);

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

#[cfg(has_lvgl)]
fn create_ui() {
    let screen = lvgl::screen_active();

    screen.bg_color(Color::black());

    // Title
    Label::create(screen)
        .text(APP_TITLE)
        .font(lvgl::font_montserrat_32())
        .color(Color::white())
        .align(lvgl::ALIGN_TOP_MID, 0, 16);

    // Counter label
    let counter = Label::create(screen)
        .text(b"Count: 0\0")
        .font(lvgl::font_montserrat_14())
        .color(Color::white())
        .align(lvgl::ALIGN_TOP_MID, 0, 64);

    // Progress bar
    let bar = Bar::create(screen)
        .size(200, 16)
        .range(0, 100)
        .value_anim(0, false)
        .indicator_color(Color::palette_main(lvgl::PALETTE_BLUE))
        .radius(8)
        .align(lvgl::ALIGN_TOP_MID, 0, 96);

    COUNTER_LABEL.init(counter);
    BAR.init(bar);
}

#[cfg(has_lvgl)]
fn ui_timer_cb() {
    let val = LAST_VALUE.load(Ordering::Relaxed);

    let _g = lvgl::lock();
    if let Some(label) = COUNTER_LABEL.try_get() {
        let mut buf = [0u8; 24];
        let mut w = FmtBuf::new(&mut buf);
        let _ = write!(w, "Count: {}", val);
        label.set_text(w.as_cstr());
    }
    if let Some(bar) = BAR.try_get() {
        bar.set_value((val % 101) as i32, true);
    }
}

// ---------------------------------------------------------------------------
// Thread entry functions (safe fn() — no unsafe extern "C" needed)
// ---------------------------------------------------------------------------

#[cfg(has_lvgl)]
fn graphics_entry() {
    loop {
        {
            let _g = lvgl::lock();
            lvgl::tick(33);
            lvgl::handler();
        }
        Thread::sleep_ms(33);
    }
}

fn producer_entry() {
    ove::log(b"[I] Producer started\n");
    let mut count: u32 = 0;

    loop {
        count += 1;

        match QUEUE.send(&count, 1000) {
            Ok(()) => {}
            Err(Error::Timeout) => {
                ove::log(b"[W] Producer: send timeout\n");
            }
            Err(Error::QueueFull) => {
                ove::log_fmt!("[W] Producer: queue full, dropped {}\n", count);
            }
            Err(_) => {
                ove::log(b"[E] Producer: unexpected send error\n");
            }
        }

        Thread::sleep_ms(500);
    }
}

fn consumer_entry() {
    ove::log(b"[I] Consumer started\n");

    loop {
        match QUEUE.receive(WAIT_FOREVER) {
            Ok(val) => {
                LAST_VALUE.store(val, Ordering::Relaxed);

                if val % 5 == 0 {
                    ove::log_fmt!("[I] Consumer: count = {}\n", val);
                }
            }
            Err(_) => {
                ove::log(b"[E] Consumer: receive error\n");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log(b"[I] Rust example: init\n");

    // Create queue
    QUEUE.init(ove::queue!(u32, 8));

    // Create threads
    #[cfg(has_lvgl)]
    let _graphics = ove::thread!("graphics", graphics_entry, Priority::High, 4096);
    let _producer = ove::thread!("producer", producer_entry, Priority::Normal, 4096);
    let _consumer = ove::thread!("consumer", consumer_entry, Priority::Normal, 4096);

    // Initialize LVGL and create UI
    #[cfg(has_lvgl)]
    {
        // Create UI timer (200ms periodic)
        UI_TIMER.init(ove::timer!(ui_timer_cb, 200, false));

        if lvgl::init().is_err() {
            ove::log(b"[E] Failed to init LVGL\n");
            return;
        }

        {
            let _g = lvgl::lock();
            create_ui();
        }
        ove::log(b"[I] LVGL widgets created\n");

        if UI_TIMER.start().is_err() {
            ove::log(b"[E] Failed to start UI timer\n");
            return;
        }
    }

    ove::log(b"[I] Rust example: ready\n");

    ove::run();

    // Cleanup (only reached if scheduler returns)
    ove::log(b"[I] Rust example: shutdown\n");
    #[cfg(has_lvgl)]
    {
        BAR.shutdown();
        COUNTER_LABEL.shutdown();
        UI_TIMER.shutdown();
    }
    QUEUE.shutdown();
}

ove::main!(app_main);
