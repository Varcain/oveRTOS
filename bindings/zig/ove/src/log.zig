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
///
/// Zig 0.16 reworked `std.io` into the vtable-based `std.Io.Writer`.
/// We expose an unbuffered (zero-length buffer) instance so each
/// `print`/`writeAll` call drains straight to `console.write` — line
/// atomicity stays the responsibility of the substrate's console
/// mutex, matching the legacy `GenericWriter` semantics.
pub const Writer = std.Io.Writer;

const writer_vtable: Writer.VTable = .{ .drain = drainToConsole };

fn drainToConsole(_: *Writer, data: []const []const u8, splat: usize) Writer.Error!usize {
    var total: usize = 0;
    if (data.len == 0) return 0;
    for (data[0 .. data.len - 1]) |slice| {
        if (slice.len > 0) console.write(slice);
        total += slice.len;
    }
    const last = data[data.len - 1];
    var i: usize = 0;
    while (i < splat) : (i += 1) {
        if (last.len > 0) console.write(last);
        total += last.len;
    }
    return total;
}

var writer_buf: [0]u8 = .{};

/// Pre-instantiated writer backed by the oveRTOS console.  Take the
/// address (`&ove.log.writer`) when passing to anything that calls
/// `print`/`writeAll` — both take `*Writer`.
pub var writer: Writer = .{
    .vtable = &writer_vtable,
    .buffer = &writer_buf,
};

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
