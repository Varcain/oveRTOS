// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
fn main() {
    println!("cargo:rustc-check-cfg=cfg(zero_heap)");
}
