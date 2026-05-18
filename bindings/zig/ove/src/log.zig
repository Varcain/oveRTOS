// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS console backend for `std.log`.
//!
//! Apps opt in at the root by setting `std_options`:
//!
//! ```zig
//! pub const std_options: std.Options = .{
//!     .logFn = ove.log.logFn,
//! };
//!
//! // Anywhere:
//! const log = std.log.scoped(.my_module);
//! log.info("count = {d}", .{val});
//! ```
//!
//! Output format matches the legacy `OVE_LOG_*` C macros so console
//! streams stay byte-compatible across the four languages:
//!
//! ```text
//! [I] [my_module] message
//! [W] [my_module] warning
//! [E] [my_module] error
//! ```
//!
//! Scope `.default` (no `scoped(.foo)` wrap) omits the scope bracket:
//!
//! ```text
//! [I] message
//! ```

const std = @import("std");
const console = @import("console.zig");

/// Writer backed by ove_console_write.  Use with std.fmt.
pub const Writer = std.io.GenericWriter(void, error{}, writeFn);

fn writeFn(_: void, bytes: []const u8) error{}!usize {
    console.write(bytes);
    return bytes.len;
}

/// Pre-instantiated writer backed by the oveRTOS console.
pub const writer = Writer{ .context = {} };

/// Format and print to the oveRTOS console.
pub fn print(comptime fmt: []const u8, args: anytype) void {
    writer.print(fmt, args) catch {};
}

/// `std.log.Options.logFn` implementation.  Wire up at the app root:
///
/// ```zig
/// pub const std_options: std.Options = .{ .logFn = ove.log.logFn };
/// ```
///
/// Output is rendered with the same `[I]`/`[W]`/`[E]`/`[D]` prefix the
/// C `OVE_LOG_*` macros use, plus a `[scope]` tag when the caller used
/// `std.log.scoped(.foo)`.  Each call uses a 256-byte stack buffer and
/// truncates silently if the formatted message exceeds it.
pub fn logFn(
    comptime level: std.log.Level,
    comptime scope: @TypeOf(.enum_literal),
    comptime fmt: []const u8,
    args: anytype,
) void {
    const prefix = comptime switch (level) {
        .err => "[E] ",
        .warn => "[W] ",
        .info => "[I] ",
        .debug => "[D] ",
    };
    const scope_tag = comptime if (scope == .default) "" else "[" ++ @tagName(scope) ++ "] ";

    var buf: [256]u8 = undefined;
    // Build the full line in a stack buffer, then emit in one
    // ove_console_write call so multi-thread interleaving is line-
    // atomic against the console mutex.
    const written = std.fmt.bufPrint(&buf, prefix ++ scope_tag ++ fmt ++ "\n", args) catch
        // Truncated — emit what we got.
        buf[0..];
    console.write(written);
}
