// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::sync::SpinMutex;

unsafe extern "C" {
    fn irq_rust_setup(kind: u32, canary: u32);
    fn irq_rust_callback();
    fn irq_rust_finish() -> i32;
}

fn arguments() -> (u32, bool, bool) {
    let args: Vec<_> = std::env::args().collect();
    assert_eq!(args.len(), 4);
    let kind = args[1].parse().expect("initialized context selector");
    let spin = args[2] == "spin";
    assert!(spin || args[2] == "global");
    (kind, spin, args[3] == "canary")
}

fn main() {
    // Argument allocations retire before a terminal native rejection can exit.
    let (kind, spin, canary) = arguments();
    unsafe { irq_rust_setup(kind, u32::from(canary)) };
    if spin {
        let owner = SpinMutex::new(7u32);
        let result = owner.with(|value| {
            *value += 1;
            unsafe { irq_rust_callback() };
            *value
        });
        assert_eq!(result, 8);
        assert_eq!(owner.into_inner(), 8);
    } else {
        critical_section::with(|_| unsafe { irq_rust_callback() });
    }
    std::process::exit(unsafe { irq_rust_finish() });
}
