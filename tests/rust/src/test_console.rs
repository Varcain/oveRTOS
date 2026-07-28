// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use std::os::unix::io::AsRawFd;

fn test_getchar_returns_none_when_empty() {
    // Save original stdin fd, redirect to /dev/null, then restore
    let saved_stdin = unsafe { libc_dup(0) };
    let devnull = std::fs::File::open("/dev/null").unwrap();
    unsafe {
        libc_dup2(devnull.as_raw_fd(), 0); // 0 = STDIN_FILENO
    }
    drop(devnull);

    let c = ove::console::getchar();
    assert!(c.is_none(), "expected None from empty console");

    // Restore original stdin
    unsafe {
        libc_dup2(saved_stdin, 0);
        libc_close(saved_stdin);
    }
}

fn test_try_getchar_returns_none_when_empty() {
    let saved_stdin = unsafe { libc_dup(0) };
    let devnull = std::fs::File::open("/dev/null").unwrap();
    unsafe {
        libc_dup2(devnull.as_raw_fd(), 0);
    }
    drop(devnull);

    assert!(ove::console::try_getchar().is_none());

    unsafe {
        libc_dup2(saved_stdin, 0);
        libc_close(saved_stdin);
    }
}

unsafe extern "C" {
    fn dup(oldfd: i32) -> i32;
    fn dup2(oldfd: i32, newfd: i32) -> i32;
    fn close(fd: i32) -> i32;
}

unsafe fn libc_dup(oldfd: i32) -> i32 {
    unsafe { dup(oldfd) }
}

unsafe fn libc_dup2(oldfd: i32, newfd: i32) {
    unsafe { dup2(oldfd, newfd) };
}

unsafe fn libc_close(fd: i32) {
    unsafe { close(fd) };
}

fn test_putchar_no_panic() {
    ove::console::putchar(b'A' as i32);
    ove::console::putchar(b'\n' as i32);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Console",
        &[
            test_entry!(test_getchar_returns_none_when_empty),
            test_entry!(test_try_getchar_returns_none_when_empty),
            test_entry!(test_putchar_no_panic),
        ],
    )
}
