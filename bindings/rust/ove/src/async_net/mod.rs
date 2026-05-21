// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async networking via embassy-net.
//!
//! Provides an `embassy_net::Stack` backed by a per-board Ethernet
//! Driver impl. Once you've spawned the runner task, open async TCP /
//! UDP / DNS sockets via the standard `embassy_net::tcp::TcpSocket` /
//! `UdpSocket` types.
//!
//! ## Build gating
//!
//! Two layers must be enabled:
//!
//! 1. **C side** — `CONFIG_OVE_ASYNC_NET=y` in your `ove_config.h`
//!    (set via `defconfig`, `menuconfig`, or an app's `app.yaml`).
//!    Implies `CONFIG_OVE_ASYNC=y`. **Mutually exclusive with
//!    `CONFIG_OVE_NET=y`** — both want the same MAC, only one IP stack
//!    can own the descriptor ring at runtime. The build system enforces
//!    this; menuconfig won't let you turn both on.
//!
//! 2. **Cargo side** — `async-net` feature on the `ove` crate, plus a
//!    transport feature for your board:
//!
//!    | Board                  | Feature                  |
//!    |------------------------|--------------------------|
//!    | qemu-mps2-an500        | `async-net-qemu-shm`     |
//!    | stm32f746g-discovery   | `async-net-stm32f7-eth`  |
//!
//!    Apps that want to build for several boards from one `Cargo.toml`
//!    can enable all transports and pick the right Driver per-board
//!    using `#[cfg(board_<name>)]` (emitted by the shared
//!    `apps/rust/ove_build_common.rs` build script).
//!
//! ## Usage
//!
//! ```ignore
//! use embassy_executor::Spawner;
//! use embassy_net::{Config, Runner, Stack};
//! use embassy_time::{Duration, Timer};
//! use static_cell::StaticCell;
//!
//! use ove::async_net::{Stm32f7EthDriver, StackResources};
//!
//! #[embassy_executor::task]
//! async fn net_task(mut runner: Runner<'static, Stm32f7EthDriver>) -> ! {
//!     runner.run().await
//! }
//!
//! #[ove::main]
//! async fn app(spawner: Spawner) {
//!     let driver = Stm32f7EthDriver::new([0x02, 0, 0, 0xBE, 0xEF, 0x01]);
//!     let config = Config::dhcpv4(Default::default());
//!     static RESOURCES: StaticCell<StackResources<3>> = StaticCell::new();
//!     let resources = RESOURCES.init(StackResources::new());
//!     let (stack, runner) = embassy_net::new(driver, config, resources, 0xdead_beef);
//!     spawner.must_spawn(net_task(runner));
//!     // ... open sockets on `stack` ...
//! }
//! ```
//!
//! A full demo with TCP heartbeats lives at
//! `apps/rust/heap/example_async_net/` (and its zero-heap twin under
//! `apps/rust/zeroheap/`). Hardware-verified on STM32F746G-Discovery
//! talking to a Pi over RMII.
//!
//! ## Comparison with the blocking [`crate::net`] stack
//!
//! Both stacks ship in the same `ove` crate; they share the [`crate::net::Address`]
//! / [`crate::Error`] types so utility code can be reused. The split is at the
//! socket / TCP/IP layer:
//!
//! | Aspect                | [`crate::net`] (blocking, lwIP/Zephyr/NuttX/POSIX) | [`crate::async_net`] (embassy-net)            |
//! |-----------------------|----------------------------------------------------|------------------------------------------------|
//! | Concurrency model     | One thread per socket loop                         | Many tasks on one executor                     |
//! | RAM footprint         | ~30-50 KB lwIP heap + per-thread stacks            | ~3 KB `StackResources<3>` + smoltcp socket bufs |
//! | TCP throughput        | Higher (lwIP zero-copy `pbuf` chains)              | Lower (smoltcp copies into TX/RX bufs)         |
//! | Latency on RX         | Driven by lwIP's tcpip_thread cadence              | ISR-woken on STM32; polled on QEMU SHM         |
//! | TLS                   | [`crate::net_tls`] (mbedTLS, in-tree)              | `embedded-tls` (crates.io)                     |
//! | HTTP client           | [`crate::net_http`] (in-tree C)                    | `reqwless` (crates.io)                         |
//! | MQTT client           | [`crate::net_mqtt`] (in-tree C)                    | `rust-mqtt` (crates.io)                        |
//! | Time sync             | [`crate::net_sntp`] (in-tree C)                    | `sntpc` (crates.io, no_std mode)               |
//! | HTTP server           | [`crate::net_httpd`] (in-tree C, REST + WS)        | `picoserve` (crates.io)                        |
//! | Cross-RTOS coverage   | All 4 backends                                     | FreeRTOS + Zephyr + NuttX on supported boards  |
//!
//! See [`crate::net`]'s module docs for the inverse decision guide.
//!
//! ## Pairing with community async crates
//!
//! Once the embassy-net Stack is up, these crates.io libraries Just Work
//! on top:
//!
//! ### TLS — [`embedded-tls`]
//!
//! ```toml
//! embedded-tls = { version = "0.17", default-features = false }
//! ```
//!
//! ```ignore
//! use embedded_tls::{Aes128GcmSha256, TlsConfig, TlsConnection, TlsContext};
//!
//! let mut tcp = TcpSocket::new(&stack, &mut rx_buf, &mut tx_buf);
//! tcp.connect(endpoint).await?;
//! let config = TlsConfig::new().with_server_name("example.com");
//! let mut tls = TlsConnection::<_, Aes128GcmSha256>::new(tcp, &mut read_record, &mut write_record);
//! tls.open(TlsContext::new(&config, UnsecureProvider::new::<Aes128GcmSha256>(rng))).await?;
//! ```
//!
//! No mbedTLS in the binary. ~50 KB code, smaller than [`crate::net_tls`].
//!
//! ### HTTP client — [`reqwless`]
//!
//! ```toml
//! reqwless = { version = "0.13", default-features = false, features = ["embedded-tls"] }
//! ```
//!
//! ```ignore
//! use reqwless::client::HttpClient;
//! use reqwless::request::Method;
//!
//! let mut tls_buf = [0u8; 16640];
//! let mut client = HttpClient::new_with_tls(&stack, &dns, TlsConfig::new(seed, &mut tls_buf, ..));
//! let mut rx_buf = [0u8; 4096];
//! let mut req = client.request(Method::GET, "https://example.com/").await?;
//! let resp = req.send(&mut rx_buf).await?;
//! ```
//!
//! Pairs with `embedded-tls` for HTTPS; works against plain HTTP without
//! it. ~30 KB code added on top of TLS.
//!
//! ### MQTT — [`rust-mqtt`]
//!
//! ```toml
//! rust-mqtt = { version = "0.3", default-features = false }
//! ```
//!
//! ```ignore
//! use rust_mqtt::client::client::MqttClient;
//! use rust_mqtt::client::client_config::ClientConfig;
//!
//! let mut tcp = TcpSocket::new(&stack, &mut rx_buf, &mut tx_buf);
//! tcp.connect(broker).await?;
//! let mut config = ClientConfig::new(MqttVersion::MQTTv5, CountingRng(seed));
//! config.add_client_id("overtos-rust");
//! let mut client = MqttClient::new(tcp, &mut write_buf, 80, &mut recv_buf, 80, config);
//! client.connect_to_broker().await?;
//! client.subscribe_to_topic("device/+/status").await?;
//! ```
//!
//! Supports MQTT 3.1.1 + 5.0; QoS 0/1/2.
//!
//! ### SNTP — [`sntpc`]
//!
//! ```toml
//! sntpc = { version = "0.4", default-features = false, features = ["embassy-socket"] }
//! ```
//!
//! Single-shot NTP query with millisecond accuracy; no separate `_init`
//! step like [`crate::net_sntp`].
//!
//! ### HTTP server — [`picoserve`]
//!
//! ```toml
//! picoserve = { version = "0.18", features = ["embassy"] }
//! ```
//!
//! Async REST router with macro-driven path matching, JSON via `serde`,
//! WebSocket upgrade. Replaces [`crate::net_httpd`] when going async.
//!
//! ## Performance notes
//!
//! - On **STM32F7 ETH** the driver runs ISR-woken — `ETH_IRQHandler`
//!   fires `HAL_ETH_RxCpltCallback` which wakes the embassy-net runner's
//!   `AtomicWaker`. No polling cadence needed for RX. The fallback poll
//!   timer (in the example app) ticks at 500 ms for slow link-state
//!   monitoring.
//! - On **QEMU MPS2-AN500 SHM** there's no interrupt; the driver
//!   polls every ~10 ms via a small timer task. RX bandwidth is gated
//!   by semihosting overhead (~1 ms per syscall), so ~100 Kbit/s ceiling.
//!   Adequate for development; for high throughput, use real hardware.
//!
//! ## Memory budget on STM32F7
//!
//! Rough costs for a typical app (DHCP + 1 TCP socket + 1 UDP socket):
//!
//! - `StackResources<3>` — 2.7 KB (in `.bss` via `static_cell::StaticCell`)
//! - smoltcp socket TX+RX bufs — 4 KB (caller-provided)
//! - ETH DMA descriptors + buffers — 6 KB (`stm32f7_eth_async.c` statics)
//! - Tasks (3) — 3 × 4 KB stacks = 12 KB (FreeRTOS heap)
//!
//! Total ~25 KB. Compare with the blocking lwIP path at ~50 KB.

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
