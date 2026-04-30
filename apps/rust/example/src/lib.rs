// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

#![cfg_attr(not(feature = "std"), no_std)]
#![deny(unsafe_code)]

use core::fmt::Write;
use core::sync::atomic::{AtomicU32, Ordering};
use ove::lvgl::{self, Bar, Color, Label, Layout, Styleable};
use ove::{Error, FmtBuf, Priority, Queue, Thread, Timer, WAIT_FOREVER};

// ---------------------------------------------------------------------------
// Constants and shared state (file-scope statics)
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
ove::shared!(UI_TIMER: Timer);
ove::shared!(COUNTER_LABEL: Label);
ove::shared!(BAR: Bar);

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

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

fn graphics_entry() {
    let mut last_us = ove::time::get_us().unwrap_or(0);

    loop {
        let now_us = ove::time::get_us().unwrap_or(last_us);
        let elapsed_ms = ((now_us - last_us) / 1000) as u32;
        last_us = now_us;

        {
            let _g = lvgl::lock();
            lvgl::tick(elapsed_ms);
            lvgl::handler();
        }
        Thread::sleep_ms(33);
    }
}

fn producer_entry() {
    ove::log_inf!("Producer started");
    let mut count: u32 = 0;

    loop {
        count += 1;

        match QUEUE.send(&count, 1000) {
            Ok(()) => {}
            Err(Error::Timeout) => {
                ove::log_wrn!("Producer: send timeout");
            }
            Err(Error::QueueFull) => {
                ove::log_wrn!("Producer: queue full, dropped {}", count);
            }
            Err(_) => {
                ove::log_err!("Producer: unexpected send error");
            }
        }

        Thread::sleep_ms(500);
    }
}

fn consumer_entry() {
    ove::log_inf!("Consumer started");

    loop {
        match QUEUE.receive(WAIT_FOREVER) {
            Ok(val) => {
                LAST_VALUE.store(val, Ordering::Relaxed);

                if val % 5 == 0 {
                    ove::log_inf!("Consumer: count = {}", val);
                }
            }
            Err(_) => {
                ove::log_err!("Consumer: receive error");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log_inf!("Rust example: init");

    // Create queue
    QUEUE.init(ove::queue!(u32, 8));

    // Create threads
    let _graphics = ove::thread!("graphics", graphics_entry, Priority::High, 4096);
    let _producer = ove::thread!("producer", producer_entry, Priority::Normal, 4096);
    let _consumer = ove::thread!("consumer", consumer_entry, Priority::Normal, 4096);

    // Create UI timer (200 ms periodic)
    UI_TIMER.init(ove::timer!(ui_timer_cb, 200, false));

    // Initialize LVGL and create UI
    if lvgl::init().is_err() {
        ove::log_err!("Failed to init LVGL");
        return;
    }

    {
        let _g = lvgl::lock();
        create_ui();
    }
    ove::log_inf!("LVGL widgets created");

    if UI_TIMER.start().is_err() {
        ove::log_err!("Failed to start UI timer");
        return;
    }

    ove::log_inf!("Rust example: ready");

    ove::run();

    ove::log_inf!("Rust example: shutdown");
}

ove::main!(app_main);
