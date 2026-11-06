// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, path::Path};

fn main() {
    let dir =
        env::var("INFER_TEST_LIB_DIR").expect("INFER_TEST_LIB_DIR must name the native fixture");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=ove_infer_test");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rerun-if-changed={dir}/libove_infer_test.a");
    println!("cargo:rerun-if-env-changed=INFER_TEST_LIB_DIR");
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
    if env::var_os("INFER_TEST_SANITIZE").is_some() {
        // Rust's address sanitizer supplies the shared Rust/C runtime.
        println!("cargo:rustc-link-lib=dylib=ubsan");
    }
    println!("cargo:rerun-if-env-changed=INFER_TEST_SANITIZE");
}
