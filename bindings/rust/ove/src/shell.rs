// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Interactive shell subsystem for oveRTOS.
//!
//! Provides [`init`], [`process_char`], and [`register_cmd`] for building a
//! simple command-line interface over a serial console. Commands are registered
//! as safe Rust `fn(args: &[&[u8]])` callbacks; the FFI trampoline handles the
//! C/Rust boundary automatically.

use crate::bindings;
use crate::error::{Error, Result};

/// Shell command handler signature.
///
/// `args` contains the parsed arguments as byte slices (argv\[0\] is the command name).
pub type CmdFn = fn(args: &[&[u8]]);

const MAX_CMDS: usize = 16;
const MAX_ARGS: usize = 8;

struct CmdEntry {
    name: &'static [u8],
    handler: CmdFn,
}

static mut CMD_TABLE: [Option<CmdEntry>; MAX_CMDS] = {
    const NONE: Option<CmdEntry> = None;
    [NONE; MAX_CMDS]
};
static mut CMD_COUNT: usize = 0;

/// Initialize the shell subsystem.
pub fn init() -> Result<()> {
    let rc = unsafe { bindings::ove_shell_init() };
    Error::from_code(rc)
}

/// Process a single input character through the shell.
pub fn process_char(c: i32) {
    unsafe { bindings::ove_shell_process_char(c) }
}

/// Register a shell command with a safe Rust handler.
///
/// `name` and `help` must be `\0`-terminated `&'static` byte slices.
/// A single trampoline dispatches to the correct handler by matching `argv[0]`.
///
/// # Errors
/// Returns [`Error::NoMemory`] when the internal command table is full
/// (capacity is 16 commands).
pub fn register_cmd(name: &'static [u8], help: &'static [u8], handler: CmdFn) -> Result<()> {
    let idx = unsafe { CMD_COUNT };
    if idx >= MAX_CMDS {
        return Err(Error::NoMemory);
    }

    unsafe {
        CMD_TABLE[idx] = Some(CmdEntry { name, handler });
        CMD_COUNT = idx + 1;
    }

    let cmd = bindings::ove_shell_cmd {
        name: name.as_ptr() as *const _,
        help: help.as_ptr() as *const _,
        handler: Some(trampoline),
    };
    let rc = unsafe { bindings::ove_shell_register_cmd(&cmd) };
    Error::from_code(rc)
}

unsafe extern "C" fn trampoline(argc: core::ffi::c_int, argv: *mut *const core::ffi::c_char) {
    if argc <= 0 || argv.is_null() {
        return;
    }

    // Build safe arg slices on the stack
    let argc = (argc as usize).min(MAX_ARGS);
    let mut args: [&[u8]; MAX_ARGS] = [&[]; MAX_ARGS];
    for (i, arg) in args.iter_mut().take(argc).enumerate() {
        let ptr = unsafe { *argv.add(i) } as *const u8;
        if !ptr.is_null() {
            *arg = unsafe { cstr_ptr_to_slice(ptr) };
        }
    }
    let args = &args[..argc];

    // Match argv[0] against registered command names
    if args.is_empty() {
        return;
    }
    let cmd_name = args[0];

    let count = unsafe { CMD_COUNT };
    for entry in unsafe { &CMD_TABLE[..count] }.iter().flatten() {
        // Compare without trailing \0
        let entry_name = strip_nul(entry.name);
        if cmd_name == entry_name {
            (entry.handler)(args);
            return;
        }
    }
}

fn strip_nul(s: &[u8]) -> &[u8] {
    if s.last() == Some(&0) {
        &s[..s.len() - 1]
    } else {
        s
    }
}

unsafe fn cstr_ptr_to_slice<'a>(ptr: *const u8) -> &'a [u8] {
    let mut len = 0;
    while unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    unsafe { core::slice::from_raw_parts(ptr, len) }
}

/// Process a complete input line through the shell.
///
/// `line` must be a null-terminated byte string (e.g. `b"help\0"`).
pub fn process_line(line: &[u8]) {
    unsafe { bindings::ove_shell_process_line(line.as_ptr() as *const _) }
}

/// Set a hook to capture shell output.
///
/// Pass `None` to remove a previously set hook.
pub fn set_output_hook(hook: bindings::ove_shell_output_hook_t) {
    unsafe { bindings::ove_shell_set_output_hook(hook) }
}
