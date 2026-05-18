// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust example — zero-heap mode.
//!
//! Showcases the static-allocation Rust pattern over the same
//! producer/consumer/UI flow as the C, C++, and heap-mode Rust examples.
//!
//!   - Caller-supplied static storage for every kernel object.  Thread
//!     storage uses [`ove::ThreadStorage<N>`], a `Sync` wrapper that the
//!     borrow checker enforces the lifetime of (no `static mut`, no
//!     `addr_of_mut!` in user code).  Queue and timer follow the
//!     established `from_static(&mut storage, …)` `unsafe` pattern
//!     until they get the same treatment.
//!   - The `ove::queue!`, `ove::thread!`, and `ove::timer!` macros each
//!     expand to a function-scope `static <storage>` declaration plus
//!     the matching constructor — they encapsulate the boilerplate
//!     while leaving the static-storage origin visible.
//!   - No `_create()` symbols are linked in this build.  No `Box`, no
//!     `alloc`, no operator new of any kind.
//!
//! LVGL specifics: the TLSF pool is pinned to LV_MEM_SIZE bytes in BSS
//! (CONFIG_LV_MEM_SIZE_KILOBYTES) with expansion disabled, so no
//! allocation ever falls back to the system malloc.  All widget creation
//! happens once in `create_ui()` before `ove::run()`; label text uses
//! `set_text_static` with caller-owned buffers so updates don't realloc.

#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicU32, Ordering};
use ove::lvgl::{self, Bar, Color, Label, Layout, Styleable};
use ove::{JoinHandle, Priority, Queue, InitCell, Thread, Timer};

// ---------------------------------------------------------------------------
// Constants and shared state
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

// `ove::shared!` is `static <NAME>: InitCell<T> = InitCell::new();`.
// In zero-heap mode it parks a wrapper whose internal storage is
// caller-owned (see app_main).
ove::shared!(QUEUE: Queue<u32, 8>);
ove::shared!(UI_TIMER: Timer);
ove::shared!(COUNTER_LABEL: Label);
ove::shared!(BAR: Bar);
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);

// Counter label text buffer — caller-owned, pinned via `set_text_static`
// so LVGL stores the pointer rather than copying into its pool.  Wrapped
// in a `SyncUnsafeCell`-style cell so we can hand out a `&'static [u8]`
// pointer to LVGL while we mutate the contents from the timer callback.
#[repr(transparent)]
struct CountBuf(core::cell::UnsafeCell<[u8; 24]>);
// SAFETY: the only writer is the UI timer callback, which runs single-
// threaded under the LVGL lock; readers (LVGL render path) only run
// under the same lock.
unsafe impl Sync for CountBuf {}
static COUNT_BUF: CountBuf =
    CountBuf(core::cell::UnsafeCell::new(*b"Count: 0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"));

fn count_buf_view() -> &'static [u8] {
    // SAFETY: we hand out a 'static reference; the buffer's address is
    // 'static (file-scope), and contents are only mutated under the
    // LVGL lock we hold elsewhere.
    unsafe { &*COUNT_BUF.0.get() }
}

// Thread handles live in static cells; their TCB + stack come from
// `ove::ThreadStorage<N>` that the `ove::thread!` macro emits as a
// `static`.  Drop never runs on a static, so the threads run forever.
// Apps can still call `.request_stop()` on the JoinHandle to signal
// cooperative shutdown.
static GFX_THREAD: InitCell<JoinHandle<()>> = InitCell::new();
static PROD_THREAD: InitCell<JoinHandle<()>> = InitCell::new();
static CONS_THREAD: InitCell<JoinHandle<()>> = InitCell::new();

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

fn create_ui() {
    let screen = lvgl::screen_active();

    screen.bg_color(Color::black());

    Label::create(screen)
        .text_static(APP_TITLE)
        .font(lvgl::font_montserrat_32())
        .color(Color::white())
        .align(lvgl::ALIGN_TOP_MID, 0, 16);

    // Pin the counter label's text to our caller-owned buffer.  After
    // create_ui() returns we only ever rewrite COUNT_BUF in place — the
    // `set_text_static` call in the timer just triggers a redraw.
    let counter = Label::create(screen)
        .text_static(count_buf_view())
        .font(lvgl::font_montserrat_14())
        .color(Color::white())
        .align(lvgl::ALIGN_TOP_MID, 0, 64);

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
    use core::fmt::Write;
    let val = LAST_VALUE.load(Ordering::Relaxed);

    let _g = lvgl::lock();
    if let Some(label) = COUNTER_LABEL.try_get() {
        // Refresh COUNT_BUF in place.  LVGL stored its pointer once at
        // create_ui() time via `text_static`; calling `set_text_static`
        // again with the *same* 'static buffer just triggers a redraw
        // — no allocation, no copy.
        // SAFETY: the timer callback is the only mutator of COUNT_BUF;
        // LVGL only reads from it under the LVGL lock we currently hold.
        let buf: &'static mut [u8; 24] = unsafe { &mut *COUNT_BUF.0.get() };
        for b in buf.iter_mut() {
            *b = 0;
        }
        let mut w = ove::FmtBuf::new(&mut buf[..]);
        let _ = write!(w, "Count: {}", val);
        label.set_text_static(count_buf_view());
    }
    if let Some(bar) = BAR.try_get() {
        bar.set_value((val % 101) as i32, true);
    }
}

// ---------------------------------------------------------------------------
// Thread entry functions (safe `fn()` — no `unsafe extern "C"`)
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
    log::info!("Producer started");
    let mut count: u32 = 0;

    loop {
        count += 1;

        match QUEUE.try_send_for(&count, core::time::Duration::from_millis(1000)) {
            Ok(()) => {}
            Err(ove::Error::Timeout) => log::warn!("Producer: send timeout"),
            Err(ove::Error::QueueFull) => {
                log::warn!("Producer: queue full, dropped {}", count)
            }
            Err(_) => log::error!("Producer: unexpected send error"),
        }

        Thread::sleep_ms(500);
    }
}

fn consumer_entry() {
    log::info!("Consumer started");

    loop {
        match QUEUE.recv() {
            Ok(val) => {
                LAST_VALUE.store(val, Ordering::Relaxed);
                if val % 5 == 0 {
                    log::info!("Consumer: count = {}", val);
                }
            }
            Err(_) => log::error!("Consumer: receive error"),
        }
    }
}

// ---------------------------------------------------------------------------
// App entry point — zero-heap uses caller-supplied static storage.
// ---------------------------------------------------------------------------

#[ove::main]
fn app_main() {
    ove::log::try_init();
    log::info!("Rust example (zero-heap mode): init");

    // The `ove::queue!` / `ove::thread!` / `ove::timer!` macros emit
    // function-scope `static <storage>: ...;` declarations and call
    // `Type::from_static(...)` against them.  In zero-heap mode this is
    // the only way to construct the wrappers (the `_create` paths are
    // not linked into the binary).
    QUEUE.init(ove::queue!(u32, 8));
    UI_TIMER.init(ove::timer!(ui_timer_cb, 200, false));

    GFX_THREAD.init(ove::thread!(
        "graphics",
        graphics_entry,
        Priority::High,
        4096
    ));
    PROD_THREAD.init(ove::thread!(
        "producer",
        producer_entry,
        Priority::Normal,
        4096
    ));
    CONS_THREAD.init(ove::thread!(
        "consumer",
        consumer_entry,
        Priority::Normal,
        4096
    ));

    if lvgl::init().is_err() {
        log::error!("Failed to init LVGL");
        return;
    }

    {
        let _g = lvgl::lock();
        create_ui();
    }
    log::info!("LVGL widgets created");

    if UI_TIMER.start().is_err() {
        log::error!("Failed to start UI timer");
        return;
    }

    log::info!("Rust example (zero-heap mode): ready");

    ove::run();

    log::info!("Rust example (zero-heap mode): shutdown");
}

