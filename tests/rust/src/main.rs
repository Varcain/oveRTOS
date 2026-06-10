// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

mod framework;

// Heap-mode suites use the `::new()` heap constructors throughout, which do
// not exist under CONFIG_OVE_ZERO_HEAP — compile them only in heap mode.  The
// static-storage path is covered by `test_zero_heap` below instead.
#[cfg(not(zero_heap))]
mod test_mutex;
#[cfg(not(zero_heap))]
mod test_mutex_data;
#[cfg(not(zero_heap))]
mod test_recursive_mutex;
#[cfg(not(zero_heap))]
mod test_semaphore;
#[cfg(not(zero_heap))]
mod test_event;
#[cfg(not(zero_heap))]
mod test_condvar;
#[cfg(not(zero_heap))]
mod test_queue;
#[cfg(not(zero_heap))]
mod test_timer;
#[cfg(not(zero_heap))]
mod test_thread;
#[cfg(not(zero_heap))]
mod test_thread_stop;
#[cfg(not(zero_heap))]
mod test_eventgroup;
#[cfg(not(zero_heap))]
mod test_time;
#[cfg(not(zero_heap))]
mod test_bsp;
#[cfg(not(zero_heap))]
mod test_board;
#[cfg(not(zero_heap))]
mod test_gpio;
#[cfg(not(zero_heap))]
mod test_led;
#[cfg(not(zero_heap))]
mod test_console;
#[cfg(not(zero_heap))]
mod test_nvs;
#[cfg(not(zero_heap))]
mod test_watchdog;
#[cfg(not(zero_heap))]
mod test_audio;
#[cfg(not(zero_heap))]
mod test_infer;
#[cfg(not(zero_heap))]
mod test_shell;
#[cfg(not(zero_heap))]
mod test_fs;
#[cfg(not(zero_heap))]
mod test_stream;
#[cfg(not(zero_heap))]
mod test_workqueue;
#[cfg(not(zero_heap))]
mod test_static_mut;
#[cfg(not(zero_heap))]
mod test_errors;
#[cfg(not(zero_heap))]
mod test_pm;
#[cfg(not(zero_heap))]
mod test_fmt;
#[cfg(not(zero_heap))]
mod test_lvgl;
#[cfg(not(zero_heap))]
mod test_embedded_hal;
#[cfg(not(zero_heap))]
mod test_net_mqtt;

// Zero-heap suite: exercises the binding's static-storage `create(&STORAGE, …)`
// constructors (D-1) that the heap suites above cannot reach.
#[cfg(zero_heap)]
mod test_zero_heap;

fn main() {
    // Install the `log` crate facade on top of `ove_console_write`.
    // `try_init` is non-fatal if already set, so re-runs of the harness
    // (e.g. cargo test with multiple binaries) are tolerated.
    ove::log::try_init();

    let mut total_passed = 0usize;
    let mut total_failed = 0usize;

    #[cfg(not(zero_heap))]
    let suites: &[(&str, fn() -> (usize, usize))] = &[
        ("Mutex", test_mutex::run),
        ("Mutex<T>", test_mutex_data::run),
        ("RecursiveMutex", test_recursive_mutex::run),
        ("Semaphore", test_semaphore::run),
        ("Event", test_event::run),
        ("CondVar", test_condvar::run),
        ("Queue", test_queue::run),
        ("Timer", test_timer::run),
        ("Thread", test_thread::run),
        ("ThreadStop", test_thread_stop::run),
        ("EventGroup", test_eventgroup::run),
        ("Time", test_time::run),
        ("BSP", test_bsp::run),
        ("Board", test_board::run),
        ("GPIO", test_gpio::run),
        ("LED", test_led::run),
        ("Console", test_console::run),
        ("NVS", test_nvs::run),
        ("Watchdog", test_watchdog::run),
        ("Audio", test_audio::run),
        ("Inference", test_infer::run),
        ("Shell", test_shell::run),
        ("Filesystem", test_fs::run),
        ("Stream", test_stream::run),
        ("Workqueue", test_workqueue::run),
        ("InitMut", test_static_mut::run),
        ("Errors", test_errors::run),
        ("PM", test_pm::run),
        ("Fmt", test_fmt::run),
        ("Lvgl", test_lvgl::run),
        ("embedded-hal+io", test_embedded_hal::run),
        ("NetMqtt", test_net_mqtt::run),
    ];

    #[cfg(zero_heap)]
    let suites: &[(&str, fn() -> (usize, usize))] = &[("ZeroHeapStatic", test_zero_heap::run)];

    for (name, runner) in suites {
        println!("\n=== Rust {} Tests ===", name);
        let (passed, failed) = runner();
        total_passed += passed;
        total_failed += failed;
    }

    println!(
        "\n=== Summary: {} passed, {} failed ===",
        total_passed, total_failed
    );

    if total_failed > 0 {
        std::process::exit(1);
    }
}
