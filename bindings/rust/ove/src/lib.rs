// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust SDK — safe wrappers for the oveRTOS embedded RTOS framework.
//!
//! Provides RAII types for threads, mutexes, semaphores, queues, timers, etc.
//! and the `app!` macro that generates all FFI boilerplate so application code
//! can be written in pure safe Rust.
//!
//! # Async (Embassy)
//!
//! Activated by the `async` Cargo feature combined with C-side
//! `CONFIG_OVE_ASYNC=y`. Adds an [Embassy](https://embassy.dev)-based
//! async runtime hosted on the oveRTOS substrate:
//!
//! ```ignore
//! use embassy_executor::Spawner;
//! use embassy_time::{Duration, Timer};
//!
//! #[embassy_executor::task]
//! async fn blinker() {
//!     loop {
//!         log::info!("tick");
//!         Timer::after(Duration::from_millis(250)).await;
//!     }
//! }
//!
//! #[ove::main]
//! async fn app_main(spawner: Spawner) {
//!     spawner.must_spawn(blinker());
//! }
//! ```
//!
//! Under the hood:
//!
//! - [`async_runtime::Executor`] wraps `embassy_executor::raw::Executor`
//!   and blocks on `ove_event_wait` between polls — yields cleanly to
//!   the FreeRTOS / Zephyr / NuttX scheduler. No `WFE` busy-park, no
//!   host-thread parking.
//! - [`async_runtime::AsyncStream`] / [`AsyncQueue`](async_runtime::AsyncQueue)
//!   / [`AsyncEventGroup`](async_runtime::AsyncEventGroup) /
//!   [`AsyncSemaphore`](async_runtime::AsyncSemaphore) /
//!   [`AsyncUart`](async_runtime::AsyncUart) /
//!   [`AsyncInput`](async_runtime::AsyncInput) wrap the corresponding
//!   synchronous primitives and bridge their C-level `_set_notify`
//!   hooks into `embassy_sync::waitqueue::AtomicWaker`.
//! - Time driver: `embassy_time::Timer::after_*()` runs on a
//!   re-armable `ove_timer_*_ns` one-shot.
//! - Critical-section impl: `ove_irq_lock` / `ove_irq_unlock` on every
//!   target.
//!
//! Cargo feature: enable `async`. The optional `embedded-io-async`
//! feature adds `embedded_io_async::Read` impls on `&'static AsyncUart`
//! and `&'static AsyncStream`.
//!
//! # Async networking (embassy-net)
//!
//! Layered on top of the async runtime, [`async_net`] adds an
//! `embassy_net::Stack` backed by a per-board Ethernet Driver
//! (`QemuShmDriver` for QEMU MPS2-AN500, `Stm32f7EthDriver` for the
//! STM32F746G-Discovery). Open async TCP / UDP / DNS sockets via the
//! standard `embassy_net` types; pair with crates.io community libraries
//! ([`reqwless`](https://crates.io/crates/reqwless),
//! [`rust-mqtt`](https://crates.io/crates/rust-mqtt),
//! [`embedded-tls`](https://crates.io/crates/embedded-tls),
//! [`picoserve`](https://crates.io/crates/picoserve)) for the protocol
//! layers.
//!
//! Mutually exclusive with the blocking [`net`] stack at build time —
//! both want to own the MAC. Cargo features: `async-net` plus a
//! transport sub-feature (`async-net-qemu-shm`, `async-net-stm32f7-eth`)
//! and C-side `CONFIG_OVE_ASYNC_NET=y`. See the [`async_net`] module
//! docs for the full decision guide vs. blocking [`net`], the
//! community-crate pairing recipes, and hardware-verified memory
//! budgets on STM32F7.
//!
//! # Hot-path inline discipline
//!
//! Wrapper methods that are a thin `unsafe { ffi::ove_*(...) }` plus
//! `Error::from_code(rc)` are marked `#[inline]` so the rustc/LLVM
//! optimizer can fold the FFI call, the error-code match, and the
//! `Result` construction into the caller's frame.  Without this,
//! `Mutex::lock` and friends compile as out-of-line `bl` targets that
//! cost an extra ~20–60 ns per call on Cortex-M and show up as a 6–15%
//! per-binding overhead on the shortest sync paths.  The same
//! discipline applies to any new wrapper added here — keep the body a
//! one-liner and add `#[inline]`.
//!
//! Cross-language inlining between Rust and C is gated behind the
//! `OVE_CROSS_LTO=ON` CMake option.  It is off by default because it
//! requires a bitcode-aware linker and a target-cpu/target-feature
//! match that the embedded toolchains do not always provide; per
//! Gale's "three quiet barriers" analysis, mismatched feature sets
//! silently kill the inliner without warning.

#![cfg_attr(not(feature = "std"), no_std)]
// Pedantic/nursery lints intentionally relaxed for this crate:
// - FFI surface (`bindings::*`) drives most pointer/cast patterns; the
//   .cast()/.cast_mut() rewrites add no safety, only churn.
// - `must_use` discipline is applied selectively on outward APIs; blanket
//   #[must_use] on every getter would be noise.
// - Documentation style follows ours, not clippy::doc_markdown's expected
//   backtick density.
#![allow(
    clippy::doc_markdown,
    clippy::must_use_candidate,
    clippy::return_self_not_must_use,
    clippy::ptr_as_ptr,
    clippy::use_self,
    clippy::borrow_as_ptr,
    clippy::ref_as_ptr,
    clippy::missing_const_for_fn,
    clippy::pub_underscore_fields,
    clippy::assertions_on_constants,
    clippy::cast_lossless,
    clippy::cast_sign_loss,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_ptr_alignment,
    clippy::ptr_cast_constness,
    clippy::not_unsafe_ptr_arg_deref,
    clippy::similar_names,
    clippy::missing_panics_doc,
    clippy::too_long_first_doc_paragraph,
    clippy::elidable_lifetime_names,
    clippy::needless_pass_by_value,
    // bindgen emits different pointer types for the same C symbol
    // depending on how LVGL's lv_obj_t is reached at parse time
    // (typedef chain vs. forward decl). Casts that look unnecessary in
    // one config are mandatory in another, so we tolerate the no-op
    // form rather than gate every callsite on cfg(docsrs).
    clippy::unnecessary_cast
)]

// Panic handler (only when no_std + feature enabled)
#[cfg(all(not(feature = "std"), feature = "panic-handler"))]
mod panic;

// Bring in `alloc` so we can re-export Arc / Box / Vec / String for
// users.  On `std` builds this is already linked transitively; on
// `no_std` the user must supply a `#[global_allocator]` (the
// `ove-allocator` sub-crate provides one that wraps libc malloc/free).
#[cfg(feature = "alloc")]
extern crate alloc;

/// Heap-allocating types re-exported from `alloc::*`.  Available with
/// the `alloc` (or `std`) feature.  On `no_std` targets the consumer
/// must register a `#[global_allocator]`; the `ove-allocator` crate
/// provides a default that wraps libc malloc/free.
#[cfg(feature = "alloc")]
pub mod heap {
    pub use alloc::{boxed::Box, string::String, vec::Vec};

    /// `Arc` re-export. By default this is `alloc::sync::Arc`; enable
    /// the `portable-atomic-arc` feature to swap in
    /// `portable_atomic_util::Arc`, which works on targets without
    /// native CAS (e.g. Cortex-M0+, RV32 without A) by falling through
    /// to a `critical-section`-based path.
    ///
    /// The two implementations are API-compatible; downstream code
    /// using `ove::heap::Arc` doesn't have to change.
    #[cfg(not(feature = "portable-atomic-arc"))]
    pub use alloc::sync::Arc;
    #[cfg(feature = "portable-atomic-arc")]
    pub use portable_atomic_util::Arc;
}

#[cfg(all(feature = "async", has_async))]
pub mod async_runtime;
// A compile-time error to catch the common misconfiguration where the
// Cargo `async` feature is on but the C side wasn't built with
// CONFIG_OVE_ASYNC=y. Without this gate the binding would fail with
// confusing linker errors about missing ove_irq_lock / ove_event_*.
#[cfg(all(feature = "async", not(has_async)))]
compile_error!(
    "feature = \"async\" requires CONFIG_OVE_ASYNC=y on the C side. \
     Enable it in your board's defconfig (or .config) and rebuild."
);
#[cfg(all(feature = "async-net", has_async_net))]
pub mod async_net;
#[cfg(all(feature = "async-net", not(has_async_net)))]
compile_error!(
    "feature = \"async-net\" requires CONFIG_OVE_ASYNC_NET=y on the C side. \
     Enable it in your board's defconfig (or .config) and rebuild. \
     Note CONFIG_OVE_NET (the blocking lwIP path) and CONFIG_OVE_ASYNC_NET \
     are mutually exclusive."
);
#[cfg(has_audio)]
pub mod audio;
#[cfg(not(docsrs))]
pub(crate) mod bindings;
#[cfg(docsrs)]
#[path = "bindings_stub.rs"]
pub(crate) mod bindings;
#[cfg(has_board)]
pub mod board;
#[cfg(has_bsp)]
pub mod bsp;
pub mod cell;
#[cfg(has_queue)]
pub mod channel;
pub mod config;
#[cfg(has_console)]
pub mod console;
pub mod containers;
#[cfg(feature = "embedded-hal")]
mod embedded_hal_impl;
#[cfg(all(feature = "embedded-io-async", feature = "async", has_async))]
mod embedded_io_async_impl;
#[cfg(feature = "embedded-io")]
mod embedded_io_impl;
pub mod error;
#[cfg(has_eventgroup)]
pub mod eventgroup;
pub mod fmt;
#[cfg(has_fs)]
pub mod fs;
#[cfg(has_gpio)]
pub mod gpio;
#[cfg(has_i2c)]
pub mod i2c;
#[cfg(has_i2s)]
pub mod i2s;
#[cfg(has_infer)]
pub mod infer;
pub mod init_cell;
#[cfg(has_led)]
pub mod led;
pub mod log;
#[cfg(has_lvgl)]
pub mod lvgl;
#[cfg(has_net)]
pub mod net;
#[cfg(has_net_http)]
pub mod net_http;
#[cfg(has_net_httpd)]
pub mod net_httpd;
#[cfg(has_net_mqtt)]
pub mod net_mqtt;
#[cfg(has_net_sntp)]
pub mod net_sntp;
#[cfg(has_net_tls)]
pub mod net_tls;
#[cfg(has_nvs)]
pub mod nvs;
#[cfg(has_pm)]
pub mod pm;
#[cfg(has_queue)]
pub mod queue;
#[cfg(has_shell)]
pub mod shell;
#[cfg(has_spi)]
pub mod spi;
#[cfg(has_stream)]
pub mod stream;
#[cfg(has_sync)]
pub mod sync;
pub mod thread;
#[cfg(has_time)]
pub mod time;
#[cfg(has_timer)]
pub mod timer;
#[cfg(has_uart)]
pub mod uart;
#[cfg(has_watchdog)]
pub mod watchdog;
#[cfg(has_workqueue)]
pub mod workqueue;

// All user-facing macros (`ove::mutex!`, `ove::thread!`, `log::info!`, etc.)
// and the internal `ove_handle_impl!` boilerplate live in `macros.rs`.
// `#[macro_export]` puts them at the crate root regardless of module.
mod macros;

/// Raw FFI bindings — **escape hatch** for narrow, app-private interop
/// that the safe wrappers don't cover (e.g. custom `lv_subject_t` observers,
/// `ove_work_fn` handlers linking against user C helpers).
///
/// **Do not use from normal app code.** App business logic should rely
/// exclusively on the safe wrappers in sibling modules (`audio`, `lvgl`,
/// `net`, `timer`, `thread`, …). Any use of `ove::ffi::*` requires the
/// surrounding module to be marked `#[allow(unsafe_code)]`, which forms
/// the de-facto auditability boundary for the app.
#[doc(hidden)]
pub mod ffi {
    pub use crate::bindings::*;
}

// Public re-exports for convenience
pub use cell::{LvCell, LvRefCell};
pub use error::{Error, Result};
#[cfg(has_eventgroup)]
#[allow(deprecated)] // EG_* shims kept for compatibility; new code uses WaitFlags
pub use eventgroup::{EG_CLEAR_ON_EXIT, EG_WAIT_ALL, EventGroup, WaitFlags};
pub use fmt::FmtBuf;
#[cfg(has_i2c)]
pub use i2c::I2c;
pub use init_cell::{InitCell, InitMut};
/// `#[ove::main]` proc-macro: marks the application entry point.
/// Expands into the `extern "C" fn ove_main()` trampoline.
pub use ove_macros::{main, thread as thread_attr};
#[cfg(has_queue)]
pub use queue::Queue;
#[cfg(has_spi)]
pub use spi::Spi;
#[cfg(has_stream)]
pub use stream::Stream;
#[cfg(has_sync)]
pub use sync::{
    CondVar, Event, Mutex, MutexGuard, RecursiveMutex, RecursiveMutexGuard, Semaphore,
    WaitTimeoutResult,
};
pub use thread::Priority;
#[cfg(zero_heap)]
pub use thread::ThreadStorage;
pub use thread::{
    Builder, JoinHandle, JoinHandleBorrowed, MemStats, StopToken, Thread, ThreadInfo, ThreadState,
    ThreadStats,
};
#[cfg(has_time)]
pub use time::Delay;
#[cfg(has_timer)]
pub use timer::Timer;
#[cfg(has_uart)]
pub use uart::Uart;
#[cfg(has_watchdog)]
pub use watchdog::Watchdog;
#[cfg(has_workqueue)]
pub use workqueue::{Work, Workqueue};

/// Write a message to the oveRTOS console.
pub fn log(msg: &[u8]) {
    // SAFETY: `msg.as_ptr()` is valid for `msg.len()` bytes. The console
    // write is a synchronous one-shot that does not retain the pointer.
    unsafe {
        bindings::ove_console_write(msg.as_ptr() as *const _, msg.len() as u32);
    }
}

/// Start audio (if enabled) and the RTOS scheduler. Blocks forever.
///
/// This function must be called at the end of `ove_main` (or the function
/// annotated with [`#[ove::main]`](main)). It transfers control to the RTOS
/// and never returns.
pub fn run() {
    // SAFETY: `ove_run` is the RTOS scheduler entry; it never returns.
    unsafe {
        bindings::ove_run();
    }
}

/// Start the RTOS scheduler **without** engaging the zero-heap lock.
///
/// Used by apps whose runtime structurally requires post-init dynamic
/// allocation — notably the benchmark suite, which spawns helper threads
/// inside test setup paths after `ove_main` has returned. On NuttX zero-heap
/// builds, the heap lock would cause `pthread_create`'s
/// `kmm_zalloc(task_group_s)` to fail with `ENOMEM`. Calling this in place
/// of [`run`] opts out of the lock.
///
/// Like [`run`], this never returns.
pub fn start_scheduler() {
    // SAFETY: kicks the scheduler entry; never returns on most platforms.
    unsafe {
        bindings::ove_thread_start_scheduler();
    }
}
