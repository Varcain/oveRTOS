// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

fn main() {
    let path = std::env::var("OVE_LVGL_TEST_LIB").expect("OVE_LVGL_TEST_LIB");
    println!("cargo:rustc-link-search=native={path}");
    println!("cargo:rustc-link-search=native={path}/lvgl/lib");
    println!("cargo:rustc-link-lib=static=ove_lvgl_text_test");
    println!("cargo:rustc-link-lib=static=lvgl");
    println!("cargo:rustc-link-lib=m");
    println!("cargo:rustc-link-lib=pthread");
    for symbol in ["lv_malloc", "lv_realloc", "lv_free"] {
        println!("cargo:rustc-link-arg=-Wl,--wrap={symbol}");
    }
    println!("cargo:rerun-if-changed={path}/libove_lvgl_text_test.a");
    println!("cargo:rerun-if-changed={path}/lvgl/lib/liblvgl.a");
    println!("cargo:rerun-if-env-changed=OVE_LVGL_TEST_LIB");
}
