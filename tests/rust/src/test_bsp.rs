// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_led_set_no_panic() {
    ove::bsp::led_set(0, true);
    ove::bsp::led_set(0, false);
    ove::bsp::led_set(1, true);
    ove::bsp::led_set(7, false);
}

fn test_led_toggle_no_panic() {
    ove::bsp::led_toggle(0);
    ove::bsp::led_toggle(0);
    ove::bsp::led_toggle(1);
}

fn test_led_set_out_of_range() {
    // Stub silently ignores out-of-range LEDs — should not panic
    ove::bsp::led_set(100, true);
    ove::bsp::led_toggle(100);
}

fn test_board_init() {
    ove::bsp::board_init().unwrap();
}

fn test_gpio_set_get() {
    ove::bsp::gpio_set(0, 0, 1).unwrap();
    let val = ove::bsp::gpio_get(0, 0).unwrap();
    assert!(val >= 0);
}

unsafe extern "C" fn irq_cb(_port: u32, _pin: u32, _user_data: *mut core::ffi::c_void) {}

fn test_gpio_irq() {
    unsafe {
        ove::bsp::gpio_irq_register(
            0, 0,
            ove::bsp::GpioIrqMode::Rising,
            Some(irq_cb),
            core::ptr::null_mut(),
        )
        .unwrap();
    }
    ove::bsp::gpio_irq_enable(0, 0).unwrap();
    ove::bsp::gpio_irq_disable(0, 0).unwrap();
    // The BSP aliases have no unregister; release the line for the suites that follow.
    ove::gpio::irq_unregister(ove::gpio::GpioPin::new(0, 0)).unwrap();
}

pub fn run() -> (usize, usize) {
    run_suite(
        "BSP",
        &[
            test_entry!(test_led_set_no_panic),
            test_entry!(test_led_toggle_no_panic),
            test_entry!(test_led_set_out_of_range),
            test_entry!(test_board_init),
            test_entry!(test_gpio_set_get),
            test_entry!(test_gpio_irq),
        ],
    )
}
