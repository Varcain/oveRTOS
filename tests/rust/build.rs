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

    println!("cargo:rerun-if-env-changed=STUB_LIB_DIR");
}
