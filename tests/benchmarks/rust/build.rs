// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

// Bench crate moved from `apps/rust/benchmark/` → `tests/benchmarks/rust/`;
// the shared rust build helper still lives next to the rust app crates
// at `apps/rust/ove_build_common.rs`.
include!("../../../apps/rust/ove_build_common.rs");

fn main() {
    ove_detect_config();

    // Benchmark configuration
    println!("cargo:rustc-check-cfg=cfg(bench_iterations)");
    println!("cargo:rustc-check-cfg=cfg(bench_warmup)");

    let gen_dir = std::env::var("OVE_GEN_DIR").unwrap_or_default();
    let config_path = format!("{}/ove_config.h", gen_dir);
    let config = std::fs::read_to_string(&config_path).unwrap_or_default();

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
}
