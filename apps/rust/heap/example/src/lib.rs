// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust example — heap mode.
//!
//! Showcases idiomatic heap-mode Rust over the same producer/consumer/UI
//! flow as the C and C++ examples — including the canonical
//! `Arc::clone(&shared) → move into closure → spawn thread` pattern that
//! "server Rust" uses.
//!
//!   - Kernel handles allocated via the binding's `Type::new()` and
//!     `Timer::new()` constructors (which call `ove_*_create` under the
//!     hood — the wrapper itself is just a heap-backed handle).
//!   - Cross-thread producer/consumer uses [`ove::channel`]'s
//!     [`Sender`](ove::channel::Sender) / [`Receiver`](ove::channel::Receiver)
//!     halves — the refcount + half-closed detection is the
//!     "MPMC channel" pattern other languages get out of the box.
//!   - Other shared state (counters, atomics) is wrapped in `Arc<T>`;
//!     threads are spawned via `Thread::spawn_with(...)` taking a
//!     `FnOnce` closure so each captures its own clone.
//!   - This exercises the full alloc stack on the target: Box (closure
//!     boxing inside `spawn_with`), Arc (refcounted shared ownership),
//!     and the `ove-allocator` crate's libc-malloc-backed
//!     `#[global_allocator]` on no_std bare-metal builds.
//!
//! Pair with `apps/rust/zeroheap/example/` which uses caller-supplied
//! `static mut` storage with the explicit `from_static` API and no
//! alloc / Arc / Box.
//!
//! LVGL operates from its own builtin TLSF pool (LV_MEM_SIZE).  In heap
//! mode the pool is allowed to grow via LV_MEM_POOL_EXPAND_SIZE; widgets
//! and label text buffers may be (re)allocated freely.

#![cfg_attr(not(feature = "std"), no_std)]

// Pull in the libc-malloc-backed `#[global_allocator]`.  On std (POSIX
// host) builds this is a no-op (`no_install`); on bare-metal targets it
// installs a `GlobalAlloc` shim wrapping libc malloc/free that routes
// to the kernel heap.
use ove_allocator as _;

use core::sync::atomic::{AtomicU32, Ordering};

use ove::channel;
use ove::heap::Arc;
use ove::lvgl::{self, Bar, Color, Label, Layout, Styleable};
use ove::{InitCell, Priority, Thread, Timer};

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

// LVGL widgets and the timer cannot be captured into closures spawned
// before they're created (chicken-and-egg with create_ui), so we still
// keep the timer/UI references in `InitCell` slots for the timer
// callback to reach them.  Everything spawn-by-Arc lives in `app_main`
// locals and is moved into the closures.
static UI_TIMER: InitCell<Timer> = InitCell::new();
static COUNTER_LABEL: InitCell<Label> = InitCell::new();
static BAR: InitCell<Bar> = InitCell::new();
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);

// ---------------------------------------------------------------------------
// LVGL UI
// ---------------------------------------------------------------------------

fn create_ui() {
    let screen = lvgl::screen_active();

    screen.bg_color(Color::black());

    Label::create(screen)
        .text(APP_TITLE)
        .font(lvgl::font_montserrat_32())
        .color(Color::white())
        .align(lvgl::ALIGN_TOP_MID, 0, 16);

    let counter = Label::create(screen)
        .text(b"Count: 0\0")
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
        let mut buf = [0u8; 24];
        let mut w = ove::FmtBuf::new(&mut buf);
        let _ = write!(w, "Count: {}", val);
        label.set_text(w.as_cstr());
    }
    if let Some(bar) = BAR.try_get() {
        bar.set_value((val % 101) as i32, true);
    }
}

// ---------------------------------------------------------------------------
// App entry point — heap mode uses Arc + spawn_with closure capture.
// ---------------------------------------------------------------------------

#[ove::main]
fn app_main() {
    ove::log::try_init();
    log::info!("Rust example (heap mode): init");

    // Construct an MPMC channel: clone Sender/Receiver into each
    // closure rather than juggling Arc<Queue> by hand. The refcount
    // lives inside the channel; we still detach the JoinHandles with
    // core::mem::forget below so they outlive app_main.
    let (tx, rx) = channel::channel::<u32, 8>().expect("channel create");
    let last_value: Arc<AtomicU32> = Arc::new(AtomicU32::new(0));

    let tx_p = tx.clone();
    let _producer = Thread::builder().name(c"producer").priority(Priority::Normal).stack_size(4096).spawn(move |_tok| {
        log::info!("Producer started");
        let mut count: u32 = 0;
        loop {
            count += 1;
            match tx_p.try_send(count) {
                Ok(()) => {}
                Err(ove::Error::Timeout) | Err(ove::Error::QueueFull) => {
                    log::warn!("Producer: queue full, dropped {}", count)
                }
                Err(ove::Error::NetClosed) => {
                    log::warn!("Producer: receivers gone");
                    break;
                }
                Err(_) => log::error!("Producer: unexpected send error"),
            }
            Thread::sleep_ms(500);
        }
    })
    .expect("producer spawn");

    let lv = Arc::clone(&last_value);
    let _consumer = Thread::builder().name(c"consumer").priority(Priority::Normal).stack_size(4096).spawn(move |_tok| {
        log::info!("Consumer started");
        loop {
            match rx.recv() {
                Ok(val) => {
                    lv.store(val, Ordering::Relaxed);
                    LAST_VALUE.store(val, Ordering::Relaxed);
                    if val % 5 == 0 {
                        log::info!("Consumer: count = {}", val);
                    }
                }
                Err(ove::Error::NetClosed) => {
                    log::warn!("Consumer: senders gone");
                    break;
                }
                Err(_) => log::error!("Consumer: receive error"),
            }
        }
    })
    .expect("consumer spawn");

    let _graphics = Thread::builder().name(c"graphics").priority(Priority::High).stack_size(4096).spawn(move |_tok| {
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
    })
    .expect("graphics spawn");

    UI_TIMER.init(Timer::new(ui_timer_cb, 200, false).expect("timer create"));

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

    // Detach the spawned thread handles so their Drop doesn't run
    // `ove_thread_destroy` when `app_main` returns.  The kernel side
    // outlives the Rust wrappers either way.
    core::mem::forget(_producer);
    core::mem::forget(_consumer);
    core::mem::forget(_graphics);
    // The Sender/Receiver clones inside each closure keep the channel
    // alive (refcount stays > 0 for the program lifetime). Drop our
    // original `tx` Sender — its closure copy is what matters.
    core::mem::forget(last_value);

    log::info!("Rust example (heap mode): ready");

    ove::run();

    log::info!("Rust example (heap mode): shutdown");
}

