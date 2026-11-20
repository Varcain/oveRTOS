// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared by the host suite and the ARM SPI fixture: pass a struct with
// enum fields and a 64-bit IRQ key to the checker in abi_roundtrip.c.
use ove::ffi;

unsafe extern "C" {
    fn ove_test_abi_spi_cfg(cfg: *const ffi::ove_spi_cfg) -> core::ffi::c_uint;
    fn ove_test_abi_irq_key(key: ffi::ove_irq_key_t) -> ffi::ove_irq_key_t;
}

pub fn check() {
    let cfg = ffi::ove_spi_cfg {
        instance: 5,
        clock_hz: 12_345_678,
        mode: ffi::OVE_SPI_MODE_3,
        bit_order: ffi::OVE_SPI_LSB_FIRST,
        word_size: 16,
    };
    // SAFETY: the checker only reads the struct during the call.
    assert_eq!(unsafe { ove_test_abi_spi_cfg(&cfg) }, 0x1f);
    let key: ffi::ove_irq_key_t = 0x1122_3344_5566_7788;
    // SAFETY: plain value call with no side effects.
    assert_eq!(unsafe { ove_test_abi_irq_key(key) }, key);
}
