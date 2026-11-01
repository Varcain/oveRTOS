// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! SPI master driver — full-duplex `transfer`, with explicit CS descriptor
//! and per-call timeout. Only the FreeRTOS STM32 driver honours the timeout,
//! in whole milliseconds rounded down; Zephyr and NuttX ignore it. Every call
//! first waits, with no timeout, for synchronous transfers through the same handle.
//! Native counts are bytes; 16-bit mode requires even counts and u16-aligned
//! buffers. Native TX/RX spans must be disjoint or exactly identical in-place.
//! The supplied software CS and same-handle lock span a sequence; native
//! hardware selection/controller locking may end between segments.
//!
//! Async calls through the raw C API notify in task context after native
//! submission/provider retirement. Keep controller backing, buffers, CS and
//! userdata valid through callback completion and every active API call's
//! return. A pending deinit/destroy consumes its handle and defers cleanup;
//! callback completion alone does not join a concurrent close. Callbacks must
//! not block. FreeRTOS/Zephyr use static periodic retry services on their
//! existing daemon/system workqueue; starvation has no bounded completion time.
//! NuttX allocates a worker TCB/stack per submission, using the bounded reserve
//! in release zero-heap mode; debug zero-heap preparation returns NotSupported.
//!
//! Wraps `ove/spi.h`. Pass `null` for `tx` to do an RX-only transfer, or
//! `null` for `rx` to do TX-only. Available when `CONFIG_OVE_SPI` is enabled.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Full-duplex SPI transfer. Pass null for tx or rx for half-duplex.
pub fn transfer(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, tx: ?[]const u8, rx: ?[]u8, timeout_ns: u64) Error!void {
    const len = if (tx) |t| t.len else if (rx) |r| r.len else 0;
    const tx_ptr = if (tx) |t| t.ptr else null;
    const rx_ptr = if (rx) |r| r.ptr else null;
    try err.fromCode(c.ove_spi_transfer(spi, cs, tx_ptr, rx_ptr, len, timeout_ns));
}

/// Write-only SPI transfer (TX only, ignore RX).
pub fn write(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, data: []const u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_spi_write(spi, cs, data.ptr, data.len, timeout_ns));
}

/// Read-only SPI transfer with backend-defined TX filler, possibly initial RX contents.
pub fn read(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, buf: []u8, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_spi_read(spi, cs, buf.ptr, buf.len, timeout_ns));
}

/// Execute a sequence of SPI transfers under a single chip-select assertion.
pub fn transferSeq(spi: c.ove_spi_t, cs: ?*const c.ove_spi_cs, xfers: []const c.ove_spi_xfer, timeout_ns: u64) Error!void {
    try err.fromCode(c.ove_spi_transfer_seq(spi, cs, xfers.ptr, @intCast(xfers.len), timeout_ns));
}
