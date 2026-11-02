// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, path::Path};

fn main() {
    let dir = env::var("ASYNC_I2C_TEST_LIB_DIR").expect("native async I2C fixture required");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=ove_async_i2c_test");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-arg=-Wl,--wrap=ove_irq_lock");
    println!("cargo:rustc-link-arg=-Wl,--wrap=ove_irq_unlock");
    println!("cargo:rerun-if-changed={dir}/libove_async_i2c_test.a");
    println!("cargo:rerun-if-env-changed=ASYNC_I2C_TEST_LIB_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
    println!("cargo:rustc-check-cfg=cfg(zero_heap)");
    let config = Path::new(&env::var("OVE_GEN_DIR").unwrap()).join("ove_config.h");
    println!("cargo:rerun-if-changed={}", config.display());
    if std::fs::read_to_string(config)
        .unwrap()
        .contains("#define CONFIG_OVE_ZERO_HEAP 1")
    {
        println!("cargo:rustc-cfg=zero_heap");
    }
    if env::var_os("ASYNC_I2C_TEST_UBSAN").is_some() {
        println!("cargo:rustc-link-lib=dylib=ubsan");
    }
    println!("cargo:rerun-if-env-changed=ASYNC_I2C_TEST_UBSAN");
}
