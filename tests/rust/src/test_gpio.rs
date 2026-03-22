// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

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

pub fn run() -> (usize, usize) {
    run_suite(
        "GPIO",
        &[
            test_entry!(test_gpio_set_get),
            test_entry!(test_gpio_irq),
            test_entry!(test_gpio_invalid_port),
        ],
    )
}
