// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust async-net demo (heap mode).
//!
//! Spawns embassy-net on top of the QEMU MPS2-AN500 shared-memory
//! transport. Three tasks:
//!
//!   - `net_task`: drives the embassy-net runner.
//!   - `poll_task`: wakes the runner every 10 ms so RX-ring polls
//!     happen even without a hardware interrupt.
//!   - `app_task`: opens a TCP socket to the host TAP gateway and
//!     writes a heartbeat every second. Logs state transitions so
//!     `make run` shows progress on the dashboard log.
//!
//! Validates the full embassy-net → QemuShmDriver → SHM ring →
//! qemu-net-bridge.py → host TAP path end-to-end.
//!
//! ## Running end-to-end
//!
//! ```sh
//! sudo config/scripts/qemu-net-setup.sh   # one-time: TAP + NAT
//! nc -lk 9999 &                            # listen on host
//! make qemu_freertos_example_async_net_rust_heap_defconfig
//! make && make run
//! ```
//!
//! The guest's static IP `172.1.1.2` matches the bridge config; the
//! host runs the bridge automatically via `qemu-run.sh`.

#![cfg_attr(not(feature = "std"), no_std)]

use ove_allocator as _;

use embassy_executor::Spawner;
use embassy_net::tcp::TcpSocket;
use embassy_net::{Config, IpAddress, IpEndpoint, Ipv4Address, Ipv4Cidr, Runner, Stack, StaticConfigV4};
use embassy_time::{Duration, Timer};

use ove::async_net::StackResources;

#[cfg(board_qemu_mps2)]
use ove::async_net::QemuShmDriver as NetDriver;
#[cfg(board_stm32f746g_disco)]
use ove::async_net::Stm32f7EthDriver as NetDriver;

#[cfg(board_qemu_mps2)]
fn poll_kick() {
    ove::async_net::qemu_shm::wake_poll();
}
#[cfg(board_stm32f746g_disco)]
fn poll_kick() {
    ove::async_net::stm32f7_eth::wake_poll();
}

const MAC_ADDR: [u8; 6] = [0x02, 0x00, 0x00, 0xBE, 0xEF, 0x01];

/// Background poller — keeps the embassy-net runner ticking.
///
/// On STM32 the ETH ISR wakes the runner on RX completion, so this is
/// only a slow link-state heartbeat. On QEMU SHM there's no ISR; the
/// poller drives every RX poll, so it ticks fast.
#[embassy_executor::task]
async fn poll_task() {
    #[cfg(board_qemu_mps2)]
    let period = Duration::from_millis(10);
    #[cfg(board_stm32f746g_disco)]
    let period = Duration::from_millis(500);
    loop {
        Timer::after(period).await;
        poll_kick();
    }
}

/// embassy-net runner driver task.
#[embassy_executor::task]
async fn net_task(mut runner: Runner<'static, NetDriver>) -> ! {
    runner.run().await
}

/// User app — opens a TCP socket to the host TAP gateway and sends a
/// heartbeat every second.
#[embassy_executor::task]
async fn app_task(stack: Stack<'static>) {
    // Wait for link-up signalled by the host bridge.
    log::info!("waiting for link...");
    while !stack.is_link_up() {
        Timer::after(Duration::from_millis(200)).await;
    }
    log::info!("link up");

    let mut rx_buf = [0u8; 1024];
    let mut tx_buf = [0u8; 1024];
    let mut counter: u32 = 0;

    loop {
        let mut socket = TcpSocket::new(stack, &mut rx_buf, &mut tx_buf);
        socket.set_timeout(Some(Duration::from_secs(5)));
        // Host gateway from qemu-net-setup.sh; listener: `nc -lk 9999`.
        let endpoint = IpEndpoint::new(IpAddress::v4(172, 1, 1, 1), 9999);
        log::info!("tick {counter}: connecting {endpoint}");
        match socket.connect(endpoint).await {
            Ok(()) => {
                let msg = b"oveRTOS async-net tick\n";
                if let Err(e) = embedded_io_async::Write::write_all(&mut socket, msg).await {
                    log::warn!("write failed: {e:?}");
                }
                socket.close();
            }
            Err(e) => log::warn!("connect failed: {e:?}"),
        }
        counter = counter.wrapping_add(1);
        Timer::after(Duration::from_secs(1)).await;
    }
}

#[ove::main]
async fn app_main(spawner: Spawner) {
    // printk! before log init: if `NetDriver::new()` panics during
    // PHY autoneg / DMA descriptor setup, this is the only line that
    // survives — the log framework isn't up yet.
    ove::printk!("oveRTOS Rust async-net demo starting\n");
    let _ = ove::log::try_init();

    let driver = NetDriver::new(MAC_ADDR);
    // Static IP matches the existing qemu-net-setup.sh layout
    // (172.1.1.0/24 with host at .1, guest at .2).
    let config = Config::ipv4_static(StaticConfigV4 {
        address: Ipv4Cidr::new(Ipv4Address::new(172, 1, 1, 2), 24),
        gateway: Some(Ipv4Address::new(172, 1, 1, 1)),
        dns_servers: Default::default(),
    });
    let seed: u64 = 0x0123_4567_89ab_cdef;

    // StaticCell rather than Box::leak: keeps the ~3 KB StackResources
    // out of the FreeRTOS heap (which fills up fast on STM32F7's tight
    // SRAM budget) and matches the zero-heap variant exactly.
    static RESOURCES: static_cell::StaticCell<StackResources<3>> = static_cell::StaticCell::new();
    let resources = RESOURCES.init(StackResources::new());
    let (stack, runner) = embassy_net::new(driver, config, resources, seed);

    spawner.must_spawn(net_task(runner));
    spawner.must_spawn(poll_task());
    spawner.must_spawn(app_task(stack));
}

