// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Tests for `FmtBuf` (zero-allocation stack formatter) and the log
//! macros that wrap it (`log_fmt!`, `log_inf!`, `log_wrn!`, `log_err!`).

use crate::framework::run_suite;
use crate::test_entry;
use core::fmt::Write;
use ove::FmtBuf;

/* ── FmtBuf::new ───────────────────────────────────────────────── */

fn test_new_seeds_null_terminator() {
    let mut buf = [0xAAu8; 8];
    let _w = FmtBuf::new(&mut buf);
    // new() must zero byte 0 so as_cstr never returns uninitialised bytes.
    assert_eq!(buf[0], 0);
    // Rest of the buffer is left untouched until write_str appends.
    assert_eq!(buf[1], 0xAA);
}

fn test_new_empty_buffer_is_noop() {
    let mut buf: [u8; 0] = [];
    let w = FmtBuf::new(&mut buf);
    assert_eq!(w.as_bytes(), b"");
}

/* ── write_str ─────────────────────────────────────────────────── */

fn test_write_single_fragment() {
    let mut buf = [0u8; 16];
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "hello").unwrap();
    assert_eq!(w.as_bytes(), b"hello");
    // as_cstr includes the trailing NUL.
    assert_eq!(w.as_cstr(), b"hello\0");
}

fn test_write_multiple_fragments() {
    let mut buf = [0u8; 32];
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "a=").unwrap();
    write!(w, "{}", 42).unwrap();
    write!(w, "/b={}", 7).unwrap();
    assert_eq!(w.as_bytes(), b"a=42/b=7");
    assert_eq!(w.as_cstr(), b"a=42/b=7\0");
}

fn test_write_maintains_null_terminator_after_each_write() {
    let mut buf = [0u8; 16];
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "foo").unwrap();
    // Byte at pos must be NUL even mid-sequence.
    assert_eq!(w.as_cstr(), b"foo\0");
    write!(w, "bar").unwrap();
    assert_eq!(w.as_cstr(), b"foobar\0");
}

fn test_write_exact_fit() {
    // Buffer of size N holds N-1 usable bytes + 1 NUL.
    let mut buf = [0u8; 6]; // room for "hello" + NUL
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "hello").unwrap();
    assert_eq!(w.as_bytes(), b"hello");
    assert_eq!(w.as_cstr(), b"hello\0");
}

fn test_write_truncates_silently() {
    let mut buf = [0u8; 6]; // capacity 5 + NUL
    let mut w = FmtBuf::new(&mut buf);
    // 10 bytes of input — must truncate to 5.
    write!(w, "1234567890").unwrap();
    assert_eq!(w.as_bytes(), b"12345");
    assert_eq!(w.as_cstr(), b"12345\0");
}

fn test_write_truncates_mid_fragment() {
    let mut buf = [0u8; 8]; // capacity 7
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "abc").unwrap();
    // Second write crosses the boundary.
    write!(w, "defghijk").unwrap();
    assert_eq!(w.as_bytes(), b"abcdefg");
    assert_eq!(w.as_cstr(), b"abcdefg\0");
}

fn test_write_empty_string_is_noop() {
    let mut buf = [0u8; 8];
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "").unwrap();
    assert_eq!(w.as_bytes(), b"");
    assert_eq!(w.as_cstr(), b"\0");
}

fn test_write_after_full_is_noop() {
    let mut buf = [0u8; 4]; // capacity 3
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "abc").unwrap();
    assert_eq!(w.as_bytes(), b"abc");
    // Subsequent writes must not corrupt the NUL or the stored bytes.
    write!(w, "XYZ").unwrap();
    assert_eq!(w.as_bytes(), b"abc");
    assert_eq!(w.as_cstr(), b"abc\0");
}

/* ── as_bytes / as_cstr accessors ──────────────────────────────── */

fn test_as_bytes_excludes_null() {
    let mut buf = [0u8; 16];
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "x").unwrap();
    let bytes = w.as_bytes();
    assert_eq!(bytes.len(), 1);
    assert_eq!(bytes, b"x");
}

fn test_as_cstr_includes_null() {
    let mut buf = [0u8; 16];
    let mut w = FmtBuf::new(&mut buf);
    write!(w, "y").unwrap();
    let cstr = w.as_cstr();
    assert_eq!(cstr.len(), 2);
    assert_eq!(cstr[cstr.len() - 1], 0);
}

/* ── Log macros ────────────────────────────────────────────────── */
// These emit to the oveRTOS console (POSIX backend = stdout). We can't
// intercept the output here without a capture hook, so we just exercise
// the macro paths to get coverage of the stack-buffer + FmtBuf flow and
// verify they compile with several argument shapes.

fn test_log_fmt_no_args() {
    ove::log_fmt!("hello\n");
}

fn test_log_fmt_with_format_args() {
    ove::log_fmt!("count = {}, flag = {}\n", 42, true);
}

fn test_log_fmt_truncation() {
    // 300-char message exceeds the 128-byte log_fmt! buffer — macro must
    // still succeed (silent truncation) without panicking.
    let big = "x".repeat(300);
    ove::log_fmt!("{}\n", big);
}

fn test_log_inf_basic() {
    ove::log_inf!("informational");
}

fn test_log_inf_with_args() {
    ove::log_inf!("Counter={}, ratio={:.2}", 7u32, 0.25f32);
}

fn test_log_wrn_basic() {
    ove::log_wrn!("warning message");
}

fn test_log_err_basic() {
    ove::log_err!("error: code={}", -42);
}

fn test_log_err_long_message_truncates() {
    // 500-char message exceeds the 256-byte _log_prefixed buffer — must
    // still run to completion with silent truncation.
    let big = "z".repeat(500);
    ove::log_err!("big={}", big);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Fmt",
        &[
            test_entry!(test_new_seeds_null_terminator),
            test_entry!(test_new_empty_buffer_is_noop),
            test_entry!(test_write_single_fragment),
            test_entry!(test_write_multiple_fragments),
            test_entry!(test_write_maintains_null_terminator_after_each_write),
            test_entry!(test_write_exact_fit),
            test_entry!(test_write_truncates_silently),
            test_entry!(test_write_truncates_mid_fragment),
            test_entry!(test_write_empty_string_is_noop),
            test_entry!(test_write_after_full_is_noop),
            test_entry!(test_as_bytes_excludes_null),
            test_entry!(test_as_cstr_includes_null),
            test_entry!(test_log_fmt_no_args),
            test_entry!(test_log_fmt_with_format_args),
            test_entry!(test_log_fmt_truncation),
            test_entry!(test_log_inf_basic),
            test_entry!(test_log_inf_with_args),
            test_entry!(test_log_wrn_basic),
            test_entry!(test_log_err_basic),
            test_entry!(test_log_err_long_message_truncates),
        ],
    )
}
