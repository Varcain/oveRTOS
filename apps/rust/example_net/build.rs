// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use std::env;

fn main() {
    let gen_dir = env::var("OVE_GEN_DIR").unwrap_or_default();
    let config_path = format!("{}/ove_config.h", gen_dir);
    let config = std::fs::read_to_string(&config_path).unwrap_or_default();

    let modules = [
        "AUDIO", "FS", "LVGL", "NVS", "SHELL", "WATCHDOG", "BSP", "BOARD",
        "GPIO", "LED", "TIME", "CONSOLE", "STREAM", "WORKQUEUE",
        "NET", "NET_TLS", "NET_HTTP", "NET_MQTT", "NET_HTTPD",
    ];
    for m in &modules {
        let cfg_name = format!("has_{}", m.to_lowercase());
        println!("cargo:rustc-check-cfg=cfg({})", cfg_name);
        let define = format!("#define CONFIG_OVE_{} 1", m);
        if config.contains(&define) {
            println!("cargo:rustc-cfg={}", cfg_name);
        }
    }

    let rtos_backends = ["FREERTOS", "ZEPHYR", "NUTTX", "POSIX"];
    for r in &rtos_backends {
        let cfg_name = format!("rtos_{}", r.to_lowercase());
        println!("cargo:rustc-check-cfg=cfg({})", cfg_name);
        let define = format!("#define CONFIG_OVE_RTOS_{} 1", r);
        if config.contains(&define) {
            println!("cargo:rustc-cfg={}", cfg_name);
        }
    }

    println!("cargo:rustc-check-cfg=cfg(zero_heap)");
    if config.contains("#define CONFIG_OVE_ZERO_HEAP 1") {
        println!("cargo:rustc-cfg=zero_heap");
    }

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
}
