// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use std::sync::atomic::{AtomicI32, Ordering};

static PING_COUNT: AtomicI32 = AtomicI32::new(0);
static ECHO_ARGC: AtomicI32 = AtomicI32::new(0);

fn test_init() {
    ove::shell::init().unwrap();
}

fn test_register_and_dispatch() {
    ove::shell::init().unwrap();

    fn ping_handler(_args: &[&[u8]]) {
        PING_COUNT.fetch_add(1, Ordering::Relaxed);
    }

    PING_COUNT.store(0, Ordering::SeqCst);
    ove::shell::register_cmd(b"ping\0", b"Ping test\0", ping_handler).unwrap();

    // Feed "ping\n" into the shell
    for &c in b"ping\n" {
        ove::shell::process_char(c as i32);
    }

    assert_eq!(
        PING_COUNT.load(Ordering::SeqCst),
        1,
        "ping handler should have been called"
    );
}

fn test_args_passed_to_handler() {
    ove::shell::init().unwrap();

    fn echo_handler(args: &[&[u8]]) {
        ECHO_ARGC.store(args.len() as i32, Ordering::Release);
    }

    ECHO_ARGC.store(0, Ordering::SeqCst);
    ove::shell::register_cmd(b"echo\0", b"Echo test\0", echo_handler).unwrap();

    // Feed "echo hello world\n"
    for &c in b"echo hello world\n" {
        ove::shell::process_char(c as i32);
    }

    assert_eq!(
        ECHO_ARGC.load(Ordering::SeqCst),
        3,
        "echo handler should receive 3 args"
    );
}

fn test_unknown_command_no_crash() {
    ove::shell::init().unwrap();
    // Feed unknown command
    for &c in b"nosuchcmd\n" {
        ove::shell::process_char(c as i32);
    }
    // Should not crash
}

fn test_multiple_commands() {
    ove::shell::init().unwrap();

    static A_COUNT: AtomicI32 = AtomicI32::new(0);
    static B_COUNT: AtomicI32 = AtomicI32::new(0);

    fn cmd_a(_args: &[&[u8]]) {
        A_COUNT.fetch_add(1, Ordering::Relaxed);
    }
    fn cmd_b(_args: &[&[u8]]) {
        B_COUNT.fetch_add(1, Ordering::Relaxed);
    }

    A_COUNT.store(0, Ordering::SeqCst);
    B_COUNT.store(0, Ordering::SeqCst);

    ove::shell::register_cmd(b"aaa\0", b"Test A\0", cmd_a).unwrap();
    ove::shell::register_cmd(b"bbb\0", b"Test B\0", cmd_b).unwrap();

    for &c in b"aaa\n" {
        ove::shell::process_char(c as i32);
    }
    for &c in b"bbb\n" {
        ove::shell::process_char(c as i32);
    }
    for &c in b"aaa\n" {
        ove::shell::process_char(c as i32);
    }

    assert_eq!(A_COUNT.load(Ordering::SeqCst), 2);
    assert_eq!(B_COUNT.load(Ordering::SeqCst), 1);
}

fn test_process_line() {
    ove::shell::init().unwrap();
    // Null-terminated line should parse through the FFI.
    ove::shell::process_line(b"\0");
    ove::shell::process_line(b"help\n\0");
}

fn test_set_output_hook() {
    // Clearing a hook must be safe even when none is set.
    ove::shell::set_output_hook(None);
}

fn test_register_cmd_table_overflow() {
    ove::shell::init().unwrap();

    fn noop(_args: &[&[u8]]) {}

    // CMD_TABLE holds at most MAX_CMDS (16) entries. Prior tests have
    // registered a handful of commands; keep registering until the Rust
    // table reports NoMemory.  Must be the last shell test — once the
    // table is full, further register_cmd calls will fail permanently.
    const NAMES: &[&[u8]] = &[
        b"f0\0", b"f1\0", b"f2\0", b"f3\0",
        b"f4\0", b"f5\0", b"f6\0", b"f7\0",
        b"f8\0", b"f9\0", b"fa\0", b"fb\0",
        b"fc\0", b"fd\0", b"fe\0", b"ff\0",
    ];
    let mut overflow_seen = false;
    for &name in NAMES {
        if let Err(e) = ove::shell::register_cmd(name, b"h\0", noop) {
            assert!(matches!(e, ove::Error::NoMemory), "unexpected: {e:?}");
            overflow_seen = true;
            break;
        }
    }
    if !overflow_seen {
        // Table wasn't full before; one more call must overflow.
        let rc = ove::shell::register_cmd(b"over\0", b"h\0", noop);
        assert!(matches!(rc, Err(ove::Error::NoMemory)), "expected NoMemory, got {:?}", rc);
    }
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Shell",
        &[
            test_entry!(test_init),
            test_entry!(test_register_and_dispatch),
            test_entry!(test_args_passed_to_handler),
            test_entry!(test_unknown_command_no_crash),
            test_entry!(test_multiple_commands),
            test_entry!(test_process_line),
            test_entry!(test_set_output_hook),
            test_entry!(test_register_cmd_table_overflow),
        ],
    )
}
