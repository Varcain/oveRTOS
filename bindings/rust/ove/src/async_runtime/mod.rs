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
//! - [`Executor`] — wraps `embassy_executor::raw::Executor` and blocks
//!   on `ove_event_wait` between polls. Yields cleanly to the
//!   underlying RTOS scheduler on FreeRTOS / Zephyr / NuttX; on POSIX
//!   blocks on a pthread condvar. Replaces the upstream `__pender`
//!   symbol with one that signals an `ove_event` via the right
//!   thread-vs-ISR variant.
//! - `critical_section::Impl` backed by `ove_irq_lock` /
//!   `ove_irq_unlock` on every target.
//! - `embassy_time_driver::Driver` backed by `ove_time_get_us` +
//!   `ove_timer_*_ns`.
//! - Async wrappers around the comm primitives ([`AsyncStream`],
//!   [`AsyncQueue`], [`AsyncEventGroup`], [`AsyncSemaphore`],
//!   [`AsyncUart`], [`AsyncInput`]) that ride on the C-level
//!   `_set_notify` hooks.

// Note: #[doc(cfg(...))] would surface the feature gate on docs.rs but
// requires nightly. Skipped to keep the crate stable-only.

pub(crate) mod critical_section;
#[cfg(has_eventgroup)]
pub mod eventgroup;
pub mod executor;
#[cfg(has_gpio)]
pub mod gpio;
#[cfg(has_queue)]
pub mod queue;
#[cfg(has_sync)]
pub mod semaphore;
#[cfg(has_stream)]
pub mod stream;
pub(crate) mod time_driver;
#[cfg(has_uart)]
pub mod uart;

#[cfg(has_eventgroup)]
pub use eventgroup::AsyncEventGroup;
pub use executor::Executor;
#[cfg(has_gpio)]
pub use gpio::AsyncInput;
#[cfg(has_queue)]
pub use queue::AsyncQueue;
#[cfg(has_sync)]
pub use semaphore::AsyncSemaphore;
#[cfg(has_stream)]
pub use stream::AsyncStream;
#[cfg(has_uart)]
pub use uart::AsyncUart;

/// Re-export of `embassy_executor::Spawner`. The Spawner returned by
/// the executor's run-loop init closure is this type.
pub use embassy_executor::Spawner;
