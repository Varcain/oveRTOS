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
        "GPIO", "LED", "TIME", "CONSOLE", "STREAM", "WORKQUEUE", "SYNC",
        "QUEUE", "TIMER", "EVENTGROUP",
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

    // Benchmark configuration
    println!("cargo:rustc-check-cfg=cfg(bench_iterations)");
    println!("cargo:rustc-check-cfg=cfg(bench_warmup)");

    // Extract iteration/warmup values as env vars for const usage
    for (define_prefix, env_name) in &[
        ("CONFIG_OVE_BENCHMARK_ITERATIONS", "OVE_BENCH_ITERATIONS"),
        ("CONFIG_OVE_BENCHMARK_WARMUP", "OVE_BENCH_WARMUP"),
    ] {
        let needle = format!("#define {} ", define_prefix);
        if let Some(pos) = config.find(&needle) {
            let rest = &config[pos + needle.len()..];
            if let Some(val) = rest.split_whitespace().next() {
                println!("cargo:rustc-env={}={}", env_name, val);
            }
        }
    }

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=OVE_GEN_DIR");
}
