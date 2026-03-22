// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Initialize the non-volatile storage subsystem.
///
/// Must be called once before any `read`, `write`, or `erase` operations.
/// Returns `Error` if the underlying flash or storage driver fails.
pub fn init() Error!void {
    try err.fromCode(c.ove_nvs_init());
}

/// Shut down the NVS subsystem and release associated resources.
pub fn deinit() void {
    c.ove_nvs_deinit();
}

/// Read the value associated with `key` into `buf`.
///
/// Returns the number of bytes actually written into `buf` on success.
/// Returns `Error.NotRegistered` if the key does not exist, or another
/// `Error` variant for I/O failures.
pub fn read(key: [*:0]const u8, buf: []u8) Error!usize {
    var out_len: usize = 0;
    try err.fromCode(c.ove_nvs_read(key, buf.ptr, buf.len, &out_len));
    return out_len;
}

/// Write `data` under the given `key`, overwriting any existing value.
///
/// Returns `Error` if the key is invalid, storage is full, or an I/O error occurs.
pub fn write(key: [*:0]const u8, data: []const u8) Error!void {
    try err.fromCode(c.ove_nvs_write(key, data.ptr, data.len));
}

/// Delete the value stored under `key`.
///
/// Returns `Error.NotRegistered` if the key does not exist, or another
/// `Error` variant for I/O failures.
pub fn erase(key: [*:0]const u8) Error!void {
    try err.fromCode(c.ove_nvs_erase(key));
}
