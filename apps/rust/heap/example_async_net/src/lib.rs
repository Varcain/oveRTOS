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
//!   - `app_task`: waits for DHCP, opens a TCP socket to a discard
//!     server (port 9), and writes a heartbeat every second. Logs
//!     state transitions so a `make qemu-run` shows progress on the
//!     dashboard log.
//!
//! Validates the full embassy-net → QemuShmDriver → SHM ring →
//! qemu-net-bridge.py → host TAP path end-to-end.

#![cfg_attr(not(feature = "std"), no_std)]

use ove_allocator as _;

use embassy_executor::Spawner;
use embassy_net::tcp::TcpSocket;
use embassy_net::{Config, IpAddress, IpEndpoint, Runner, Stack};
use embassy_time::{Duration, Timer};

use ove::async_net::{QemuShmDriver, StackResources};

const MAC_ADDR: [u8; 6] = [0x02, 0x00, 0x00, 0xBE, 0xEF, 0x01];

/// Background poller — wakes the embassy-net runner every 10 ms so the
/// SHM-ring RX path advances (no interrupt source for QEMU SHM).
#[embassy_executor::task]
async fn poll_task() {
    loop {
        Timer::after(Duration::from_millis(10)).await;
        ove::async_net::qemu_shm::wake_poll();
    }
}

/// embassy-net runner driver task.
#[embassy_executor::task]
async fn net_task(mut runner: Runner<'static, QemuShmDriver>) -> ! {
    runner.run().await
}

/// User app — waits for DHCP, then opens a TCP socket and sends a
/// heartbeat every second.
#[embassy_executor::task]
async fn app_task(stack: Stack<'static>) {
    log::info!("waiting for DHCP...");
    loop {
        if let Some(cfg) = stack.config_v4() {
            log::info!("DHCP up: ip={} gw={:?}", cfg.address, cfg.gateway);
            break;
        }
        Timer::after(Duration::from_millis(500)).await;
    }

    let mut rx_buf = [0u8; 1024];
    let mut tx_buf = [0u8; 1024];
    let mut counter: u32 = 0;

    loop {
        let mut socket = TcpSocket::new(stack, &mut rx_buf, &mut tx_buf);
        socket.set_timeout(Some(Duration::from_secs(5)));
        // Talk to whatever the host bridge exposes — typical setup:
        // tap0 = 172.31.0.1, host runs `nc -lk 9999` on the tap.
        let endpoint = IpEndpoint::new(IpAddress::v4(172, 31, 0, 1), 9999);
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
    let _ = ove::log::try_init();
    log::info!("oveRTOS Rust async-net demo starting");

    // Driver + Stack. Use ::leak to give the Stack 'static buffers
    // (heap mode); zero-heap would use static_cell macros instead.
    let driver = QemuShmDriver::new(MAC_ADDR);
    let config = Config::dhcpv4(Default::default());
    let seed: u64 = 0x0123_4567_89ab_cdef;

    let resources = alloc::boxed::Box::leak(alloc::boxed::Box::new(StackResources::<3>::new()));
    let (stack, runner) = embassy_net::new(driver, config, resources, seed);

    spawner.must_spawn(net_task(runner));
    spawner.must_spawn(poll_task());
    spawner.must_spawn(app_task(stack));
}

extern crate alloc;
