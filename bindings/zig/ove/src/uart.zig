// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! UART driver — `write` / `read` with per-call timeouts, plus
//! `bytesAvailable` and `flush` helpers.
//!
//! Wraps `ove/uart.h`. RX is async-buffered by the backend; reads return as
//! soon as any bytes are available (or block up to `timeout_ns`). Available
//! when `CONFIG_OVE_UART` is enabled.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Write data to the UART. Returns the number of bytes written.
pub fn write(uart: c.ove_uart_t, data: []const u8, timeout_ns: u64) Error!usize {
    var written: usize = 0;
    try err.fromCode(c.ove_uart_write(uart, data.ptr, data.len, timeout_ns, &written));
    return written;
}

/// Read data from the UART RX buffer. Returns the number of bytes read.
pub fn read(uart: c.ove_uart_t, buf: []u8, timeout_ns: u64) Error!usize {
    var read_count: usize = 0;
    try err.fromCode(c.ove_uart_read(uart, buf.ptr, buf.len, timeout_ns, &read_count));
    return read_count;
}

/// Query the number of bytes available in the RX buffer.
pub fn bytesAvailable(uart: c.ove_uart_t) usize {
    return c.ove_uart_bytes_available(uart);
}

/// Flush the TX hardware buffer.
pub fn flush(uart: c.ove_uart_t) Error!void {
    try err.fromCode(c.ove_uart_flush(uart));
}
