// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust networking example — zero-heap mode (placeholder).
//!
//! The full networking test suite uses runtime allocation paths inside
//! the lwIP / TLS / HTTP / MQTT clients that cannot be expressed
//! statically.  See apps/rust/heap/example_net/ for the full demo.

#![cfg_attr(not(feature = "std"), no_std)]

use ove::{Priority, Thread};

fn net_thread() {
    ove::log_inf!("net (zero-heap): see apps/rust/heap/example_net for the full demo");
    loop {
        Thread::sleep_ms(10000);
    }
}

fn app_main() {
    ove::log_inf!("Rust networking example (zero-heap mode): stub");
    let _net = ove::thread!("net-stub", net_thread, Priority::Normal, 4096);
    ove::run();
}

ove::main!(app_main);
