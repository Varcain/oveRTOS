// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Interactive shell subsystem for oveRTOS.
//!
//! Provides [`init`], [`process_char`], and [`register_cmd`] for building a
//! simple command-line interface over a serial console. Commands are safe
//! Rust handlers taking `args: &[&[u8]]`: function items or closures that
//! capture nothing, each given its own C trampoline.
//!
//! The binding keeps no state of its own; the C shell's table and lock do
//! the rest, so every function here may be called from any thread (never
//! from an ISR). Commands run one at a time with the shell's lock held: a
//! handler may register commands or dispatch a line itself, but must not
//! wait for another thread that is using the shell.

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

const MAX_ARGS: usize = 8;

/// Initialize the shell subsystem.
///
/// Empties the command table; only the built-in `help` command remains.
///
/// # Errors
/// Returns the error from creating the shell's lock on first use
/// ([`Error::NoMemory`] when a heap build cannot allocate it).
pub fn init() -> Result<()> {
    let rc = unsafe { bindings::ove_shell_init() };
    Error::from_code(rc)
}

/// Process a single input character through the shell.
///
/// Call it from thread context, never from an ISR: the matching command's
/// handler runs inside the call that delivers the newline. Feed characters
/// from one thread, as the line being typed is shared. If the shell's lock
/// cannot be created on first use, `c` is dropped.
pub fn process_char(c: i32) {
    unsafe { bindings::ove_shell_process_char(c) }
}

/// Register a shell command with a safe Rust handler.
///
/// `handler` is a function item or a closure that captures nothing, e.g.
/// `register_cmd(c"ping", c"Ping", ping)`; it is called with the parsed
/// arguments, `args[0]` being the command name, on whichever thread feeds
/// the shell, so it must be `Send` and `Sync`. Passing a function pointer
/// value or a capturing closure fails to build (`cargo check` does not
/// catch it), as the handler is recovered from its type alone. Registering
/// a name again replaces its handler and help text.
///
/// # Errors
/// Returns [`Error::NoMemory`] when the command table is full (16 commands,
/// including the built-in `help`) or, on first use, a heap build cannot
/// allocate the shell's lock.
pub fn register_cmd<F>(name: &'static CStr, help: &'static CStr, _handler: F) -> Result<()>
where
    F: Fn(&[&[u8]]) + Copy + Send + Sync + 'static,
{
    const { assert_zero_sized::<F>() };
    let cmd = bindings::ove_shell_cmd {
        name: name.as_ptr(),
        help: help.as_ptr(),
        handler: Some(trampoline::<F>),
    };
    Error::from_code(unsafe { bindings::ove_shell_register_cmd(&cmd) })
}

const fn assert_zero_sized<F>() {
    assert!(
        core::mem::size_of::<F>() == 0,
        "shell handlers must be fn items or closures that capture nothing"
    );
}

unsafe extern "C" fn trampoline<F>(argc: core::ffi::c_int, argv: *mut *const core::ffi::c_char)
where
    F: Fn(&[&[u8]]) + Copy + Send + Sync + 'static,
{
    const { assert_zero_sized::<F>() };
    let mut args: [&[u8]; MAX_ARGS] = [&[]; MAX_ARGS];
    // SAFETY: the C shell passes its argv for the duration of this call.
    let argc = unsafe { collect_args(argc, argv, &mut args) };
    if argc == 0 {
        return; // the C shell never dispatches an empty line
    }
    // SAFETY: `F` is zero-sized (checked above), so it has no bytes and no
    // invalid values; it is `Copy + Send + 'static`, so making one here is
    // the same as copying the value `register_cmd` was given to this thread.
    let handler: F = unsafe { core::mem::zeroed() };
    handler(&args[..argc]);
}

/// The per-handler trampolines share this, so each adds only its call.
#[inline(never)]
unsafe fn collect_args(
    argc: core::ffi::c_int,
    argv: *mut *const core::ffi::c_char,
    args: &mut [&[u8]; MAX_ARGS],
) -> usize {
    if argc <= 0 || argv.is_null() {
        return 0;
    }
    let argc = (argc as usize).min(MAX_ARGS);
    for (i, arg) in args.iter_mut().take(argc).enumerate() {
        let ptr = unsafe { *argv.add(i) } as *const u8;
        if !ptr.is_null() {
            *arg = unsafe { cstr_ptr_to_slice(ptr) };
        }
    }
    argc
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
/// Thread context only, from any thread or a command handler; it does not
/// touch the line [`process_char`] is building. At most 127 characters are
/// used. If the shell's lock cannot be created on first use, the line is
/// dropped.
pub fn process_line(line: &CStr) {
    unsafe { bindings::ove_shell_process_line(line.as_ptr()) }
}

/// Process a line, passing its command's output to `hook`.
///
/// For this one command `hook` takes the place of the hook set with
/// [`set_output_hook`]: it gets each log line the command emits from its own
/// thread while it runs, and no other command's. The hook runs on the calling
/// thread with the shell's lock held. A hook the command itself sets with
/// [`set_output_hook`] stays afterwards, unless it is `hook` itself.
pub fn process_line_with_hook(line: &CStr, hook: bindings::ove_shell_output_hook_t) {
    unsafe { bindings::ove_shell_process_line_with_hook(line.as_ptr(), hook) }
}

/// Set a hook to capture shell output.
///
/// Pass `None` to remove a previously set hook. Setting it takes the shell's
/// lock, so it waits for a running command (thread context only). The hook
/// runs on the command's thread with the shell's lock held, so it must not
/// wait for another thread that is using the shell.
pub fn set_output_hook(hook: bindings::ove_shell_output_hook_t) {
    unsafe { bindings::ove_shell_set_output_hook(hook) }
}
