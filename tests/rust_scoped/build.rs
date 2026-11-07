// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, path::Path};
fn main() {
    let dir =
        env::var("SCOPED_TEST_LIB_DIR").expect("SCOPED_TEST_LIB_DIR names the native fixture");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=ove_scoped_test");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rerun-if-changed={dir}/libove_scoped_test.a");
    println!("cargo:rerun-if-env-changed=SCOPED_TEST_LIB_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
    let path = Path::new(&env::var("OVE_GEN_DIR").unwrap()).join("ove_config.h");
    println!("cargo:rerun-if-changed={}", path.display());
    let config = std::fs::read_to_string(path).unwrap();
    for (name, cfg) in [
        ("ZERO_HEAP", "zero_heap"),
        ("TIME", "has_time"),
        ("ASYNC", "has_async"),
    ] {
        println!("cargo:rustc-check-cfg=cfg({cfg})");
        if config.contains(&format!("#define CONFIG_OVE_{name} 1")) {
            println!("cargo:rustc-cfg={cfg}");
        }
    }
    println!("cargo:rustc-check-cfg=cfg(scoped_parent)");
    if env::var_os("SCOPED_TEST_PARENT").is_some() {
        println!("cargo:rustc-cfg=scoped_parent");
    }
    println!("cargo:rerun-if-env-changed=SCOPED_TEST_PARENT");
    if env::var_os("SCOPED_TEST_SANITIZE").is_some() {
        println!("cargo:rustc-link-lib=dylib=ubsan");
    }
    println!("cargo:rerun-if-env-changed=SCOPED_TEST_SANITIZE");
}
