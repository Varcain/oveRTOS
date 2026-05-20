// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Embassy-based async runtime hosted on the oveRTOS C substrate.
//!
//! Activated by the `async` Cargo feature combined with C-side
//! `CONFIG_OVE_ASYNC=y` (build.rs detects this and emits
//! `cfg(has_async)`). The module provides:
//!
//! - The `embassy_time_driver::Driver` backed by `ove_time_get_us`
//!   and a re-armed one-shot `ove_timer_*_ns` alarm.
//! - (On embedded arch features) a `critical_section::Impl` backed
//!   by `ove_irq_lock` / `ove_irq_unlock`. On `async-arch-std` the
//!   built-in `critical-section/std` impl from upstream is used
//!   instead.
//! - A re-export of the upstream `embassy_executor::Executor` and
//!   `Spawner` types so users don't need to add `embassy-executor` to
//!   their dep manifest directly (though they may, for example to use
//!   the `#[task]` macro).
//!
//! The Phase 1 vertical slice relies on embassy-executor's own
//! `__pender` and run-loop implementations: on POSIX (`arch-std`) the
//! executor blocks on a `Condvar`; on Cortex-M (`arch-cortex-m`) it
//! uses WFE. A future revision will provide an oveRTOS-native run-loop
//! that blocks on `ove_event_wait` so the executor cooperates cleanly
//! with the RTOS scheduler on FreeRTOS / Zephyr / NuttX targets where
//! multiple threads compete for the CPU.

#![cfg_attr(docsrs, doc(cfg(feature = "async")))]

#[cfg(feature = "async-custom-cs")]
pub(crate) mod critical_section;
#[cfg(has_eventgroup)]
pub mod eventgroup;
#[cfg(has_queue)]
pub mod queue;
#[cfg(has_sync)]
pub mod semaphore;
#[cfg(has_stream)]
pub mod stream;
pub(crate) mod time_driver;

#[cfg(has_eventgroup)]
pub use eventgroup::AsyncEventGroup;
#[cfg(has_queue)]
pub use queue::AsyncQueue;
#[cfg(has_sync)]
pub use semaphore::AsyncSemaphore;
#[cfg(has_stream)]
pub use stream::AsyncStream;

/// Re-export so users can write `use ove::async_runtime::Executor;`
/// without a direct dependency on `embassy_executor`.
pub use embassy_executor::Executor;
/// Re-export so users can write `use ove::async_runtime::Spawner;`.
pub use embassy_executor::Spawner;
