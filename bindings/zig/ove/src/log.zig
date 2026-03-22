// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const console = @import("console.zig");

/// Writer backed by ove_console_write. Use with std.fmt.
pub const Writer = std.io.GenericWriter(void, error{}, writeFn);

fn writeFn(_: void, bytes: []const u8) error{}!usize {
    console.write(bytes);
    return bytes.len;
}

/// A ready-to-use `Writer` instance backed by the oveRTOS console.
///
/// Pass to `std.fmt.format()` or any function accepting a `std.io.AnyWriter`
/// for zero-allocation formatted output to the console.
pub const writer = Writer{ .context = {} };

/// Convenience: format and print to ove console.
pub fn print(comptime fmt: []const u8, args: anytype) void {
    writer.print(fmt, args) catch {};
}
