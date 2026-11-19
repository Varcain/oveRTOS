// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Layout contract: the size probe compiles cmake/ove_abi_types.def with
//! the target's C compiler; every struct size and alignment and each
//! field's offset and size it measured must match what @cImport sees.

const std = @import("std");
const c = @import("c.zig").raw;

fn lower(comptime s: []const u8) []const u8 {
    comptime var buf: [s.len]u8 = undefined;
    inline for (s, 0..) |ch, i| buf[i] = std.ascii.toLower(ch);
    const out = buf;
    return &out;
}

fn check(comptime what: []const u8, comptime zig: usize, comptime gcc: usize) []const u8 {
    if (zig == gcc) return "";
    return std.fmt.comptimePrint("\n  {s} is {d} in Zig but {d} in C", .{ what, zig, gcc });
}

comptime {
    @setEvalBranchQuota(1_000_000);
    var bad: []const u8 = "";
    for (@typeInfo(c).@"struct".decls) |d| {
        const n = d.name;
        if (std.mem.startsWith(u8, n, "OVE_SIZEOF_STRUCT_")) {
            const tag = lower(n["OVE_SIZEOF_STRUCT_".len..]);
            bad = bad ++ check("sizeof " ++ tag, @sizeOf(@field(c, "struct_" ++ tag)), @field(c, n));
        } else if (std.mem.startsWith(u8, n, "OVE_ALIGNOF_STRUCT_")) {
            const tag = lower(n["OVE_ALIGNOF_STRUCT_".len..]);
            bad = bad ++ check("alignof " ++ tag, @alignOf(@field(c, "struct_" ++ tag)), @field(c, n));
        } else if (std.mem.startsWith(u8, n, "OVE_OFFSETOF_")) {
            const key = n["OVE_OFFSETOF_".len..];
            const sep = std.mem.indexOf(u8, key, "__").?;
            const tag = lower(key[0..sep]);
            const field = lower(key[sep + 2 ..]);
            bad = bad ++ check("offsetof " ++ tag ++ "." ++ field, @offsetOf(@field(c, "struct_" ++ tag), field), @field(c, n));
        } else if (std.mem.startsWith(u8, n, "OVE_FIELDSIZE_")) {
            const key = n["OVE_FIELDSIZE_".len..];
            const sep = std.mem.indexOf(u8, key, "__").?;
            const tag = lower(key[0..sep]);
            const field = lower(key[sep + 2 ..]);
            bad = bad ++ check("sizeof " ++ tag ++ "." ++ field, @sizeOf(@FieldType(@field(c, "struct_" ++ tag), field)), @field(c, n));
        }
    }
    if (bad.len != 0) @compileError("C ABI mismatch between @cImport and the C compiler:" ++ bad);
}
