// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust async-net demo — zero-heap mode.
//!
//! Mirror of `apps/rust/heap/example_async_net/` running with
//! `CONFIG_OVE_ZERO_HEAP=y` and no `alloc` feature. The Stack
//! resources sit in a function-scope `static_cell::StaticCell`
//! rather than a leaked Box.

#![cfg_attr(not(feature = "std"), no_std)]

use embassy_executor::Spawner;
use embassy_net::tcp::TcpSocket;
use embassy_net::{Config, IpAddress, IpEndpoint, Ipv4Address, Ipv4Cidr, Runner, Stack, StaticConfigV4};
use embassy_time::{Duration, Timer};
use static_cell::StaticCell;

use ove::async_net::{QemuShmDriver, StackResources};

const MAC_ADDR: [u8; 6] = [0x02, 0x00, 0x00, 0xBE, 0xEF, 0x02];

#[embassy_executor::task]
async fn poll_task() {
    loop {
        Timer::after(Duration::from_millis(10)).await;
        ove::async_net::qemu_shm::wake_poll();
    }
}

#[embassy_executor::task]
async fn net_task(mut runner: Runner<'static, QemuShmDriver>) -> ! {
    runner.run().await
}

#[embassy_executor::task]
async fn app_task(stack: Stack<'static>) {
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
    let _ = ove::log::try_init();
    log::info!("oveRTOS Rust async-net demo (zero-heap) starting");

    let driver = QemuShmDriver::new(MAC_ADDR);
    let config = Config::ipv4_static(StaticConfigV4 {
        address: Ipv4Cidr::new(Ipv4Address::new(172, 1, 1, 2), 24),
        gateway: Some(Ipv4Address::new(172, 1, 1, 1)),
        dns_servers: Default::default(),
    });
    let seed: u64 = 0xfeed_face_dead_beef;

    static RESOURCES: StaticCell<StackResources<3>> = StaticCell::new();
    let resources = RESOURCES.init(StackResources::new());
    let (stack, runner) = embassy_net::new(driver, config, resources, seed);

    spawner.must_spawn(net_task(runner));
    spawner.must_spawn(poll_task());
    spawner.must_spawn(app_task(stack));
}
