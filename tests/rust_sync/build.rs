// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, path::Path};

fn main() {
    let dir =
        env::var("SYNC_TEST_LIB_DIR").expect("SYNC_TEST_LIB_DIR must name the native fixture");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=ove_sync_test");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rerun-if-changed={dir}/libove_sync_test.a");
    println!("cargo:rerun-if-env-changed=SYNC_TEST_LIB_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
    println!("cargo:rustc-check-cfg=cfg(zero_heap)");
    println!("cargo:rustc-check-cfg=cfg(sync_optional)");
    let config = Path::new(&env::var("OVE_GEN_DIR").unwrap()).join("ove_config.h");
    println!("cargo:rerun-if-changed={}", config.display());
    let config = std::fs::read_to_string(config).unwrap();
    let optional = env::var_os("CARGO_FEATURE_OPTIONAL").is_some();
    assert_eq!(
        optional,
        config.contains("#define CONFIG_OVE_TIME 1")
            && config.contains("#define CONFIG_OVE_ASYNC 1"),
        "optional proof dependencies must match TIME and ASYNC configuration"
    );
    if config.contains("#define CONFIG_OVE_ZERO_HEAP 1") {
        println!("cargo:rustc-cfg=zero_heap");
    }
    if config.contains("#define CONFIG_OVE_TIME 1") {
        println!("cargo:rustc-cfg=sync_optional");
    }
    if env::var_os("SYNC_TEST_SANITIZE").is_some() {
        println!("cargo:rustc-link-lib=dylib=ubsan");
    }
    println!("cargo:rerun-if-env-changed=SYNC_TEST_SANITIZE");
}
