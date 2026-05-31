// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use std::env;

fn main() {
    let stub_lib_dir =
        env::var("STUB_LIB_DIR").expect("STUB_LIB_DIR must be set (path to libove_stub.a)");

    println!("cargo:rustc-link-search=native={}", stub_lib_dir);
    println!("cargo:rustc-link-lib=static:+whole-archive=ove_stub");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=rt");

    // Mirror bindings/rust/ove/build.rs: enable the `zero_heap` cfg (which
    // gates the test crate's zero-heap suite) when OVE_GEN_DIR/ove_config.h
    // selects the static-storage mode. cfgs are per-crate, so the test crate
    // needs its own emission even though the ove crate emits the same signal.
    println!("cargo:rustc-check-cfg=cfg(zero_heap)");
    if let Ok(gen_dir) = env::var("OVE_GEN_DIR") {
        let cfg = std::path::Path::new(&gen_dir).join("ove_config.h");
        println!("cargo:rerun-if-changed={}", cfg.display());
        if std::fs::read_to_string(&cfg)
            .map(|s| s.contains("#define CONFIG_OVE_ZERO_HEAP 1"))
            .unwrap_or(false)
        {
            println!("cargo:rustc-cfg=zero_heap");
        }
    }

    println!("cargo:rerun-if-env-changed=STUB_LIB_DIR");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
}
