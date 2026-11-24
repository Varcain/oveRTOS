// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

mod framework;
mod test_semaphore;

unsafe extern "C" {
    fn ove_freertos_posix_run(suite: extern "C" fn() -> i32) -> i32;
}

extern "C" fn run_suite() -> i32 {
    // During scheduling, only the runner may print or allocate through libc in test code.
    println!("\n=== Rust Semaphore Tests ===");
    let (passed, failed) = test_semaphore::run();
    println!("\n=== Summary: {passed} passed, {failed} failed ===");
    i32::from(failed != 0)
}

fn main() {
    std::process::exit(unsafe { ove_freertos_posix_run(run_suite) });
}
