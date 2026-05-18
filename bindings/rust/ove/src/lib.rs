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
    pub use alloc::{boxed::Box, string::String, sync::Arc, vec::Vec};
}

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
#[cfg(has_console)]
pub mod console;
pub mod containers;
#[cfg(feature = "embedded-hal")]
mod embedded_hal_impl;
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
pub mod init_cell;
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
#[cfg(has_queue)]
pub use queue::Queue;
#[cfg(has_spi)]
pub use spi::Spi;
#[cfg(has_time)]
pub use time::Delay;
#[cfg(has_uart)]
pub use uart::Uart;
pub use init_cell::{InitCell, InitMut};
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
#[cfg(has_timer)]
pub use timer::Timer;
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
/// passed to the [`main!`] macro). It transfers control to the RTOS and
/// never returns.
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
