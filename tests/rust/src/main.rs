// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

mod framework;
mod test_mutex;
mod test_recursive_mutex;
mod test_semaphore;
mod test_event;
mod test_condvar;
mod test_queue;
mod test_timer;
mod test_thread;
mod test_eventgroup;
mod test_time;
mod test_bsp;
mod test_board;
mod test_gpio;
mod test_led;
mod test_console;
mod test_nvs;
mod test_watchdog;
mod test_audio;
mod test_shell;
mod test_fs;
mod test_stream;
mod test_workqueue;
mod test_static_mut;

fn main() {
    let mut total_passed = 0usize;
    let mut total_failed = 0usize;

    let suites: &[(&str, fn() -> (usize, usize))] = &[
        ("Mutex", test_mutex::run),
        ("RecursiveMutex", test_recursive_mutex::run),
        ("Semaphore", test_semaphore::run),
        ("Event", test_event::run),
        ("CondVar", test_condvar::run),
        ("Queue", test_queue::run),
        ("Timer", test_timer::run),
        ("Thread", test_thread::run),
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
        ("Shell", test_shell::run),
        ("Filesystem", test_fs::run),
        ("Stream", test_stream::run),
        ("Workqueue", test_workqueue::run),
        ("StaticMut", test_static_mut::run),
    ];

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
