// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use std::env;

fn main() {
    let directory =
        env::var("EVENT_WAITER_TEST_LIB_DIR").expect("native observer library directory");
    let library = env::var("EVENT_WAITER_TEST_LIB").expect("native observer library name");
    println!("cargo:rustc-link-search=native={directory}");
    println!("cargo:rustc-link-lib=static={library}");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rerun-if-changed={directory}/lib{library}.a");
    println!("cargo:rerun-if-env-changed=EVENT_WAITER_TEST_LIB_DIR");
    println!("cargo:rerun-if-env-changed=EVENT_WAITER_TEST_LIB");
}
