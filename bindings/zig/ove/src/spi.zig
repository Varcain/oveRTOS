// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Full-duplex SPI transfer. Pass null for tx or rx for half-duplex.
pub fn transfer(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, tx: ?[]const u8, rx: ?[]u8, timeout_ms: u32) Error!void {
    const len = if (tx) |t| t.len else if (rx) |r| r.len else 0;
    const tx_ptr = if (tx) |t| t.ptr else null;
    const rx_ptr = if (rx) |r| r.ptr else null;
    try err.fromCode(c.ove_spi_transfer(spi, cs, tx_ptr, rx_ptr, len, timeout_ms));
}

/// Write-only SPI transfer (TX only, ignore RX).
pub fn write(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, data: []const u8, timeout_ms: u32) Error!void {
    try err.fromCode(c.ove_spi_write(spi, cs, data.ptr, data.len, timeout_ms));
}

/// Read-only SPI transfer (clock out zeros, capture RX).
pub fn read(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, buf: []u8, timeout_ms: u32) Error!void {
    try err.fromCode(c.ove_spi_read(spi, cs, buf.ptr, buf.len, timeout_ms));
}

/// Execute a sequence of SPI transfers under a single chip-select assertion.
pub fn transferSeq(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, xfers: []const c.ove_spi_xfer, timeout_ms: u32) Error!void {
    try err.fromCode(c.ove_spi_transfer_seq(spi, cs, xfers.ptr, @intCast(xfers.len), timeout_ms));
}
