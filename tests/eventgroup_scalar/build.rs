// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
fn main() {
    let dir = std::env::var("EVENTGROUP_TEST_LIB_DIR").unwrap();
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=eventgroup_binding");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rerun-if-env-changed=EVENTGROUP_TEST_LIB_DIR");
    println!("cargo:rerun-if-changed={dir}/libeventgroup_binding.a");
}
