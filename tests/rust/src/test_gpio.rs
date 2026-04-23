// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::gpio::{GpioIrqMode, GpioMode, GpioPin};

/* ── BSP-level (thin wrapper) ───────────────────────────────────────── */

fn test_gpio_set_get() {
    ove::bsp::gpio_set(0, 0, 1).unwrap();
    let val = ove::bsp::gpio_get(0, 0).unwrap();
    assert!(val >= 0);
}

fn test_gpio_irq() {
    unsafe {
        ove::bsp::gpio_irq_register(
            0, 0,
            ove::bsp::GpioIrqMode::Rising,
            None,
            core::ptr::null_mut(),
        )
        .unwrap();
    }
    ove::bsp::gpio_irq_enable(0, 0).unwrap();
    ove::bsp::gpio_irq_disable(0, 0).unwrap();
}

fn test_gpio_invalid_port() {
    let rc = ove::bsp::gpio_set(9999, 9999, 1);
    assert!(rc.is_err());
}

/* ── High-level ove::gpio API ───────────────────────────────────────── */

fn test_gpio_pin_descriptor() {
    let p = GpioPin::new(2, 5);
    assert_eq!(p.port, 2);
    assert_eq!(p.pin, 5);

    // Clone/Copy/Eq derives
    let q = p;
    assert_eq!(p, q);
    let r = GpioPin::new(2, 6);
    assert!(p != r);
}

fn test_gpio_high_level_configure_set_get() {
    let p = GpioPin::new(0, 0);
    ove::gpio::configure(p, GpioMode::OutputPP).unwrap();
    ove::gpio::set(p, 1).unwrap();
    let val = ove::gpio::get(p).unwrap();
    assert!(val >= 0);
    ove::gpio::set(p, 0).unwrap();
}

fn test_gpio_high_level_modes() {
    let p = GpioPin::new(0, 1);
    ove::gpio::configure(p, GpioMode::Input).unwrap();
    ove::gpio::configure(p, GpioMode::OutputOD).unwrap();
    ove::gpio::configure(p, GpioMode::OutputPP).unwrap();
}

fn test_gpio_high_level_irq() {
    let p = GpioPin::new(0, 0);
    unsafe {
        ove::gpio::irq_register(p, GpioIrqMode::Rising, None, core::ptr::null_mut()).unwrap();
    }
    ove::gpio::irq_enable(p).unwrap();
    ove::gpio::irq_disable(p).unwrap();

    let p2 = GpioPin::new(0, 1);
    unsafe {
        ove::gpio::irq_register(p2, GpioIrqMode::Falling, None, core::ptr::null_mut()).unwrap();
    }
    let p3 = GpioPin::new(0, 2);
    unsafe {
        ove::gpio::irq_register(p3, GpioIrqMode::Both, None, core::ptr::null_mut()).unwrap();
    }
}

fn test_gpio_high_level_invalid_port() {
    let p = GpioPin::new(9999, 9999);
    assert!(ove::gpio::set(p, 1).is_err());
}

pub fn run() -> (usize, usize) {
    run_suite(
        "GPIO",
        &[
            test_entry!(test_gpio_set_get),
            test_entry!(test_gpio_irq),
            test_entry!(test_gpio_invalid_port),
            test_entry!(test_gpio_pin_descriptor),
            test_entry!(test_gpio_high_level_configure_set_get),
            test_entry!(test_gpio_high_level_modes),
            test_entry!(test_gpio_high_level_irq),
            test_entry!(test_gpio_high_level_invalid_port),
        ],
    )
}
