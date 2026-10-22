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
//!
//! The command table, like the C table it mirrors, is not synchronised: call
//! [`init`] and [`register_cmd`] from one thread, not concurrently with
//! [`process_char`] or [`process_line`]. A handler may itself register commands.

use core::cell::UnsafeCell;
use core::ffi::CStr;

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY (module-wide contract for the `unsafe { bindings::ove_*(...) }` FFI
// calls below): any handle passed to the C API is non-null and refers to a
// live RTOS object — wrapper constructors establish validity via
// `Error::from_code`, and `Drop` (or an explicit `deinit`) is the only place
// a handle is released. Pointer and slice arguments reference caller-owned
// memory valid for the duration of the call; the C side copies whatever it
// retains and does not alias them past return (verified against the
// signatures in `include/ove/*.h`). Blocks that deviate — `transmute`, raw
// pointer casts from user data, slice reconstruction via `from_raw_parts`,
// or storing a callback across the FFI boundary — carry their own
// `// SAFETY:` comment.

/// Shell command handler signature.
///
/// `args` contains the parsed arguments as byte slices (argv\[0\] is the command name).
pub type CmdFn = fn(args: &[&[u8]]);

const MAX_CMDS: usize = 16;
const MAX_ARGS: usize = 8;

struct CmdEntry {
    name: &'static CStr,
    handler: CmdFn,
}

struct CmdTable {
    entries: [Option<CmdEntry>; MAX_CMDS],
    count: usize,
}

struct Registry(UnsafeCell<CmdTable>);

// SAFETY: the table is only touched from the one shell thread (module-level
// contract above), exactly like the C table it mirrors.
unsafe impl Sync for Registry {}

static REGISTRY: Registry = Registry(UnsafeCell::new(CmdTable {
    entries: [const { None }; MAX_CMDS],
    count: 0,
}));

/// Initialize the shell subsystem.
///
/// Empties the command table; only the built-in `help` command remains.
pub fn init() -> Result<()> {
    // SAFETY: single-threaded registration per the module contract. The C
    // side empties its table, so drop the Rust mirror with it.
    unsafe { (*REGISTRY.0.get()).count = 0 };
    let rc = unsafe { bindings::ove_shell_init() };
    Error::from_code(rc)
}

/// Process a single input character through the shell.
///
/// Call it from thread context, never from an ISR: the matching command's
/// handler runs inside the call that delivers the newline.
pub fn process_char(c: i32) {
    unsafe { bindings::ove_shell_process_char(c) }
}

/// Register a shell command with a safe Rust handler.
///
/// A single trampoline dispatches to the correct handler by matching `argv[0]`.
/// Registering a name again replaces its handler and help text.
///
/// # Errors
/// Returns [`Error::NoMemory`] when the command table is full (16 commands,
/// including the built-in `help`).
pub fn register_cmd(name: &'static CStr, help: &'static CStr, handler: CmdFn) -> Result<()> {
    // SAFETY: single-threaded registration per the module contract, and the
    // C call below does not re-enter this module.
    let table = unsafe { &mut *REGISTRY.0.get() };
    let count = table.count;
    let existing = table.entries[..count]
        .iter()
        .position(|e| e.as_ref().is_some_and(|e| e.name == name));
    let slot = match existing {
        Some(i) => i,
        None if count < MAX_CMDS => count,
        None => return Err(Error::NoMemory),
    };

    let cmd = bindings::ove_shell_cmd {
        name: name.as_ptr(),
        help: help.as_ptr(),
        handler: Some(trampoline),
    };
    // Commit to the Rust table only once C has accepted the command, so the
    // two tables cannot disagree.
    Error::from_code(unsafe { bindings::ove_shell_register_cmd(&cmd) })?;
    table.entries[slot] = Some(CmdEntry { name, handler });
    if slot == count {
        table.count = count + 1;
    }
    Ok(())
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

    // Copy the handler out so the table borrow ends before it runs: a
    // handler may register commands.
    // SAFETY: dispatch runs on the shell thread per the module contract.
    let table = unsafe { &*REGISTRY.0.get() };
    let handler = table.entries[..table.count]
        .iter()
        .flatten()
        .find(|e| e.name.to_bytes() == cmd_name)
        .map(|e| e.handler);
    if let Some(handler) = handler {
        handler(args);
    }
}

unsafe fn cstr_ptr_to_slice<'a>(ptr: *const u8) -> &'a [u8] {
    let mut len = 0;
    // SAFETY: caller (the `trampoline`) guarantees `ptr` is a non-null,
    // NUL-terminated C string from `argv`; the scan stops at the NUL, and the
    // resulting slice covers exactly the bytes before it.
    while unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    unsafe { core::slice::from_raw_parts(ptr, len) }
}

/// Process a complete input line through the shell (e.g. `c"help"`).
///
/// Like [`process_char`], call it from thread context only.
pub fn process_line(line: &CStr) {
    unsafe { bindings::ove_shell_process_line(line.as_ptr()) }
}

/// Set a hook to capture shell output.
///
/// Pass `None` to remove a previously set hook.
pub fn set_output_hook(hook: bindings::ove_shell_output_hook_t) {
    unsafe { bindings::ove_shell_set_output_hook(hook) }
}
