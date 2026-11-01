// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

fn main() {
    let dir = std::env::var("OVE_STRING_TEST_LIB").unwrap();
    println!("cargo:rustc-link-search=native={dir}");
    for lib in ["ove_stub", "ove_mbedtls"] {
        println!("cargo:rustc-link-lib=static:+whole-archive={lib}");
        println!("cargo:rerun-if-changed={dir}/lib{lib}.a");
    }
    println!("cargo:rustc-link-lib=pthread");
    println!("cargo:rustc-link-lib=rt");
    println!("cargo:rustc-link-lib=m");
    println!("cargo:rustc-check-cfg=cfg(zero_heap)");
    let config = std::env::var("OVE_GEN_DIR").unwrap() + "/ove_config.h";
    if std::fs::read_to_string(&config)
        .unwrap()
        .contains("#define CONFIG_OVE_ZERO_HEAP 1")
    {
        println!("cargo:rustc-cfg=zero_heap");
    }
    println!("cargo:rerun-if-changed={config}");
    println!("cargo:rerun-if-env-changed=OVE_STRING_TEST_LIB");
}
