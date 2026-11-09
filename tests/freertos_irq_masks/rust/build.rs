// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
fn main() {
    println!("cargo:rerun-if-env-changed=OVE_IRQ_MODEL_OBJECT");
    let object = std::env::var("OVE_IRQ_MODEL_OBJECT").expect("native model object");
    println!("cargo:rerun-if-changed={object}");
    println!("cargo:rustc-link-arg={object}");
}
