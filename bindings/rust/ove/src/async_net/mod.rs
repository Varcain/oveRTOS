// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async networking via embassy-net.
//!
//! Gated by the `async-net` Cargo feature combined with a transport
//! sub-feature (e.g. `async-net-qemu-shm`). The C side must be built
//! with `CONFIG_OVE_ASYNC_NET=y`; the existing blocking `ove::net::*`
//! stack and embassy-net are mutually exclusive at build time because
//! both want the same MAC / transport.
//!
//! ## Usage
//!
//! ```ignore
//! use ove::async_net::{QemuShmDriver, Stack, StackResources};
//! use embassy_net::Config;
//!
//! #[ove::main]
//! async fn app(spawner: Spawner) {
//!     let driver = QemuShmDriver::new([0x02, 0, 0, 0xBE, 0xEF, 0x01]);
//!     let config = Config::dhcpv4(Default::default());
//!     static RESOURCES: StackResources<3> = StackResources::new();
//!     let (stack, runner) = embassy_net::new(driver, config, &RESOURCES, 0x1234_5678);
//!     spawner.must_spawn(net_task(runner));
//!     // ... open sockets on `stack` ...
//! }
//! ```

#[cfg(has_async_net)]
pub mod qemu_shm;
#[cfg(has_async_net)]
pub mod stm32f7_eth;

#[cfg(has_async_net)]
pub use qemu_shm::QemuShmDriver;
#[cfg(has_async_net)]
pub use stm32f7_eth::Stm32f7EthDriver;

/// Re-export of [`embassy_net::Stack`].
pub use embassy_net::Stack;
/// Re-export of [`embassy_net::StackResources`].
pub use embassy_net::StackResources;
