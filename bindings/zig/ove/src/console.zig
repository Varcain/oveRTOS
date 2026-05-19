// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Console UART I/O — init, byte/slice write, formatted print, and blocking read.
//!
//! Wraps `ove/console.h`. `print()` integrates with `std.fmt` for inline
//! formatting; routed to the board's default UART once `init()` has run.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Initialize the oveRTOS console driver.
///
/// Must be called before `write()`, `putchar()`, or `getchar()`.
/// Returns `Error` if the UART or other console hardware fails to initialize.
pub fn init() Error!void {
    try err.fromCode(c.ove_console_init());
}

/// Write a byte slice to the console output.
///
/// Blocks until all bytes have been transmitted. Suitable for bulk output.
pub fn write(buf: []const u8) void {
    c.ove_console_write(buf.ptr, @intCast(buf.len));
}

/// Write a single character to the console output.
pub fn putchar(ch: u8) void {
    c.ove_console_putchar(@intCast(ch));
}

/// Read one character from the console input without blocking.
///
/// Returns `null` if no character is available. Returns the character as a
/// `u8` if one is ready in the receive buffer.
pub fn getchar() ?u8 {
    const ch = c.ove_console_getchar();
    if (ch < 0) return null;
    return @intCast(ch);
}
