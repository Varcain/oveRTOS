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
    ove::shell::register_cmd(c"ping", c"Ping test", ping_handler).unwrap();

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
    ove::shell::register_cmd(c"echo", c"Echo test", echo_handler).unwrap();

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

    ove::shell::register_cmd(c"aaa", c"Test A", cmd_a).unwrap();
    ove::shell::register_cmd(c"bbb", c"Test B", cmd_b).unwrap();

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
    ove::shell::process_line(c"");
    ove::shell::process_line(c"help\n");
}

fn test_set_output_hook() {
    // Clearing a hook must be safe even when none is set.
    ove::shell::set_output_hook(None);
}

/// Re-registering a name must replace the handler, matching the C table
/// (ove_shell_register_cmd overwrites a same-name entry).  Regression: the
/// Rust side appended a second entry and its trampoline dispatched the
/// FIRST match, so the stale handler kept running.
fn test_register_same_name_replaces_handler() {
    ove::shell::init().unwrap();

    static DUP_A: AtomicI32 = AtomicI32::new(0);
    static DUP_B: AtomicI32 = AtomicI32::new(0);

    fn dup_a(_args: &[&[u8]]) {
        DUP_A.fetch_add(1, Ordering::Relaxed);
    }
    fn dup_b(_args: &[&[u8]]) {
        DUP_B.fetch_add(1, Ordering::Relaxed);
    }

    DUP_A.store(0, Ordering::SeqCst);
    DUP_B.store(0, Ordering::SeqCst);
    ove::shell::register_cmd(c"dup", c"first", dup_a).unwrap();
    ove::shell::register_cmd(c"dup", c"second", dup_b).unwrap();

    for &c in b"dup\n" {
        ove::shell::process_char(c as i32);
    }

    let (a, b) = (DUP_A.load(Ordering::SeqCst), DUP_B.load(Ordering::SeqCst));
    println!("    dup: old handler ran {a}x, new handler ran {b}x (expect 0/1)");
    assert_eq!(b, 1, "re-registered handler should run");
    assert_eq!(a, 0, "replaced handler must not run");
}

fn test_register_cmd_table_overflow() {
    ove::shell::init().unwrap();

    fn noop(_args: &[&[u8]]) {}

    // The table holds 16 commands including the built-in `help`; keep
    // registering until it reports NoMemory.
    const NAMES: &[&core::ffi::CStr] = &[
        c"f0", c"f1", c"f2", c"f3",
        c"f4", c"f5", c"f6", c"f7",
        c"f8", c"f9", c"fa", c"fb",
        c"fc", c"fd", c"fe", c"ff",
    ];
    let mut overflow_seen = false;
    for &name in NAMES {
        if let Err(e) = ove::shell::register_cmd(name, c"h", noop) {
            assert!(matches!(e, ove::Error::NoMemory), "unexpected: {e:?}");
            overflow_seen = true;
            break;
        }
    }
    if !overflow_seen {
        // Table wasn't full before; one more call must overflow.
        let rc = ove::shell::register_cmd(c"over", c"h", noop);
        assert!(matches!(rc, Err(ove::Error::NoMemory)), "expected NoMemory, got {:?}", rc);
    }
}

/// `init()` empties the C command table, so after a re-init there must be
/// room again.  Regression: the Rust CMD_TABLE/CMD_COUNT survived init()
/// and kept returning NoMemory although the C table was empty.
fn test_reinit_frees_command_table() {
    ove::shell::init().unwrap();

    fn noop(_args: &[&[u8]]) {}

    // Fill the table to capacity.
    const NAMES: &[&core::ffi::CStr] = &[
        c"g0", c"g1", c"g2", c"g3",
        c"g4", c"g5", c"g6", c"g7",
        c"g8", c"g9", c"ga", c"gb",
        c"gc", c"gd", c"ge", c"gf",
    ];
    for &name in NAMES {
        if ove::shell::register_cmd(name, c"h", noop).is_err() {
            break;
        }
    }

    ove::shell::init().unwrap();
    let rc = ove::shell::register_cmd(c"after", c"h", noop);
    println!("    register after re-init: {rc:?} (expect Ok)");
    assert!(rc.is_ok(), "register after re-init should succeed, got {rc:?}");
}

/// A handler that dispatches a line keeps its own arguments: the nested
/// line used to overwrite the buffer they point into ("hello" became
/// "XXXXX").
fn test_nested_dispatch_keeps_args() {
    static INNER: AtomicI32 = AtomicI32::new(0);
    static INTACT: AtomicI32 = AtomicI32::new(0);

    fn inner(_args: &[&[u8]]) {
        INNER.fetch_add(1, Ordering::Relaxed);
    }
    fn outer(args: &[&[u8]]) {
        ove::shell::register_cmd(c"inner", c"nested", inner).unwrap();
        ove::shell::process_line(c"inner XXXXXXXX YYYYYYYY");
        INTACT.store(i32::from(args.get(1) == Some(&&b"hello"[..])), Ordering::Relaxed);
    }

    ove::shell::init().unwrap();
    INNER.store(0, Ordering::SeqCst);
    INTACT.store(0, Ordering::SeqCst);
    ove::shell::register_cmd(c"outer", c"nests", outer).unwrap();
    ove::shell::process_line(c"outer hello");
    assert_eq!(INNER.load(Ordering::SeqCst), 1);
    assert_eq!(INTACT.load(Ordering::SeqCst), 1);
}

/// Registering and dispatching from two threads through the safe API: the
/// binding keeps no table of its own to race on.
fn test_threads_register_and_dispatch() {
    static HITS: AtomicI32 = AtomicI32::new(0);

    fn hit(_args: &[&[u8]]) {
        HITS.fetch_add(1, Ordering::Relaxed);
    }

    ove::shell::init().unwrap();
    HITS.store(0, Ordering::SeqCst);
    ove::shell::register_cmd(c"hit", c"count", hit).unwrap();
    std::thread::scope(|s| {
        s.spawn(|| {
            for _ in 0..500 {
                ove::shell::register_cmd(c"hit", c"count again", hit).unwrap();
            }
        });
        s.spawn(|| {
            for _ in 0..500 {
                ove::shell::process_line(c"hit");
            }
        });
    });
    assert_eq!(HITS.load(Ordering::SeqCst), 500);
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
            test_entry!(test_register_same_name_replaces_handler),
            test_entry!(test_register_cmd_table_overflow),
            test_entry!(test_reinit_frees_command_table),
            test_entry!(test_nested_dispatch_keeps_args),
            test_entry!(test_threads_register_and_dispatch),
        ],
    )
}
