// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! I²C master driver — write, read, and write-then-read transactions plus a
//! `probe` helper for bus scanning.
//!
//! Wraps `ove/i2c.h`. Timeouts are nanoseconds; pass `OVE_WAIT_FOREVER` for an
//! indefinite block. Available when `CONFIG_OVE_I2C` is enabled.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Write data to an I2C device.
pub fn write(i2c: c.ove_i2c_t, addr: u16, data: []const u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_i2c_write(i2c, addr, data.ptr, data.len, timeout_ns));
}

/// Read data from an I2C device.
pub fn read(i2c: c.ove_i2c_t, addr: u16, buf: []u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_i2c_read(i2c, addr, buf.ptr, buf.len, timeout_ns));
}

/// Combined write-then-read with I2C repeated start.
pub fn writeRead(i2c: c.ove_i2c_t, addr: u16, tx: []const u8, rx: []u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_i2c_write_read(i2c, addr, tx.ptr, tx.len, rx.ptr, rx.len, timeout_ns));
}

/// Write to a single-byte-addressed register.
pub fn regWrite(i2c: c.ove_i2c_t, addr: u16, reg: u8, data: []const u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_i2c_reg_write(i2c, addr, reg, data.ptr, data.len, timeout_ns));
}

/// Read from a single-byte-addressed register.
pub fn regRead(i2c: c.ove_i2c_t, addr: u16, reg: u8, buf: []u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_i2c_reg_read(i2c, addr, reg, buf.ptr, buf.len, timeout_ns));
}

/// Probe for a device at the given address.
pub fn probe(i2c: c.ove_i2c_t, addr: u16, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_i2c_probe(i2c, addr, timeout_ns));
}
