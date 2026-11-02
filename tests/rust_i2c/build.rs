// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, path::Path};
fn main() {
    let dir = env::var("I2C_TEST_LIB_DIR").expect("I2C_TEST_LIB_DIR must name the native fixture");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=ove_i2c_test");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rerun-if-changed={dir}/libove_i2c_test.a");
    println!("cargo:rerun-if-env-changed=I2C_TEST_LIB_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
    let path = Path::new(&env::var("OVE_GEN_DIR").unwrap()).join("ove_config.h");
    println!("cargo:rerun-if-changed={}", path.display());
    let config = std::fs::read_to_string(path).unwrap();
    for (module, cfg) in [
        ("ZERO_HEAP", "zero_heap"),
        ("TIME", "has_time"),
        ("CONSOLE", "has_console"),
    ] {
        println!("cargo:rustc-check-cfg=cfg({cfg})");
        if config.contains(&format!("#define CONFIG_OVE_{module} 1")) {
            println!("cargo:rustc-cfg={cfg}");
        }
    }
    if env::var_os("I2C_TEST_SANITIZE").is_some() {
        println!("cargo:rustc-link-lib=dylib=ubsan");
    }
    println!("cargo:rerun-if-env-changed=I2C_TEST_SANITIZE");
}
