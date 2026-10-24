// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Interactive command shell — register handlers, dispatch incoming bytes,
//! print prompts.
//!
//! Wraps `ove/shell.h`. Each registered command is identified by a
//! null-terminated name; the handler receives an argv array. Available when
//! `CONFIG_OVE_SHELL` is enabled.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Initialize the interactive shell subsystem.
///
/// Must be called before `registerCmd` or `processChar`.
/// Returns `Error` if the underlying shell driver fails to start.
pub fn init() Error!void {
    try err.fromCode(c.ove_shell_init());
}

/// Register a new shell command.
///
/// - `name_`: null-terminated command name (e.g. `"reboot"`).
/// - `help`: null-terminated one-line help string displayed by the built-in help command.
/// - `handler`: called when the command is entered. Receives standard `argc`/`argv`
///   arguments. Must not block indefinitely.
///
/// The shell keeps the `name_` and `help` pointers, not copies: both strings
/// must stay valid while the command is registered. Registering a name again
/// replaces that command, even when the table is full.
///
/// Returns `Error` if registration fails (e.g. command table is full).
pub fn registerCmd(
    name_: [*:0]const u8,
    help: [*:0]const u8,
    comptime handler: fn (argc: c_int, argv: [*c]const [*c]const u8) void,
) Error!void {
    const Trampoline = struct {
        fn invoke(argc: c_int, argv: [*c]const [*c]const u8) callconv(.c) void {
            handler(argc, argv);
        }
    };

    var cmd: c.struct_ove_shell_cmd = .{
        .name = name_,
        .help = help,
        .handler = &Trampoline.invoke,
    };
    try err.fromCode(c.ove_shell_register_cmd(&cmd));
}

/// Feed a single character into the shell input parser.
///
/// Call this from thread context, typically a console polling loop, never
/// from an ISR: the shell accumulates characters and runs the matching
/// command handler inside the call that delivers the newline. Feed
/// characters from one thread, as the line being typed is shared;
/// `processLine` has its own copy and may run on any thread.
pub fn processChar(ch: u8) void {
    c.ove_shell_process_char(@intCast(ch));
}

/// Process a complete input line through the shell.
///
/// Tokenises the line and dispatches to the matching command handler.
/// More convenient than `processChar` for programmatic use (e.g. WebSocket terminal).
/// Thread context only, from any thread or a command handler; it does not
/// touch the line `processChar` is building. At most 127 characters are used.
pub fn processLine(line: [*:0]const u8) void {
    c.ove_shell_process_line(line);
}

/// Set a hook to capture shell output.
///
/// When set, shell command output (normally printed to console) is also
/// forwarded to the hook. Pass `null` to remove. The hook runs on the
/// command's thread with the shell's lock held, so it must not wait for
/// another thread that is using the shell.
pub fn setOutputHook(hook: c.ove_shell_output_hook_t) void {
    c.ove_shell_set_output_hook(hook);
}
