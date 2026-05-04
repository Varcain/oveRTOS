// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const c = @import("c.zig").raw;

/// Error set representing all possible oveRTOS failure codes.
///
/// Maps from the C-layer integer error codes to typed Zig errors.
pub const Error = error{
    /// The requested resource or command has not been registered.
    NotRegistered,
    /// An argument or configuration value is out of range or invalid.
    InvalidParam,
    /// Dynamic memory allocation failed (heap exhausted).
    NoMemory,
    /// A blocking operation timed out before completing.
    Timeout,
    /// The requested feature is not available on this platform or configuration.
    NotSupported,
    /// A message could not be enqueued because the queue is at capacity.
    QueueFull,
    /// The remote end refused the connection.
    NetRefused,
    /// The destination network or host is unreachable.
    NetUnreachable,
    /// The requested local address is already in use.
    NetAddrInUse,
    /// The connection was reset by the remote end.
    NetReset,
    /// DNS name resolution failed.
    NetDnsFail,
    /// The connection has been closed by the peer.
    NetClosed,
    /// An unrecognized error code was returned by the C layer.
    Unknown,
};

/// Sentinel value for `timeout_ms` parameters meaning "block indefinitely".
pub const wait_forever: u32 = c.OVE_WAIT_FOREVER;

inline fn mapErrorCode(rc: c_int) Error {
    return switch (rc) {
        c.OVE_ERR_NOT_REGISTERED => Error.NotRegistered,
        c.OVE_ERR_INVALID_PARAM => Error.InvalidParam,
        c.OVE_ERR_NO_MEMORY => Error.NoMemory,
        c.OVE_ERR_TIMEOUT => Error.Timeout,
        c.OVE_ERR_NOT_SUPPORTED => Error.NotSupported,
        c.OVE_ERR_QUEUE_FULL => Error.QueueFull,
        c.OVE_ERR_NET_REFUSED => Error.NetRefused,
        c.OVE_ERR_NET_UNREACHABLE => Error.NetUnreachable,
        c.OVE_ERR_NET_ADDR_IN_USE => Error.NetAddrInUse,
        c.OVE_ERR_NET_RESET => Error.NetReset,
        c.OVE_ERR_NET_DNS_FAIL => Error.NetDnsFail,
        c.OVE_ERR_NET_CLOSED => Error.NetClosed,
        else => Error.Unknown,
    };
}

/// Convert a negative C error code to a Zig error.
/// For functions that return a non-negative value on success (e.g. node index),
/// use `fromCodeInt` instead.
pub inline fn fromCode(rc: c_int) Error!void {
    if (rc >= 0) return;
    return mapErrorCode(rc);
}

/// Convert a C return code that carries a value on success.
/// Returns the value if rc >= 0, or the mapped error if rc < 0.
pub inline fn fromCodeInt(rc: c_int) Error!c_int {
    if (rc >= 0) return rc;
    return mapErrorCode(rc);
}

// Compile-time assertion: the C `OVE_ERR_*` defines this binding maps in
// `mapErrorCode` must keep the negative numeric codes that Zig callers
// rely on.  If the C header renumbers a code, bindgen-via-@cImport
// regenerates a different constant and this block fails to compile.
// Analog of Rust's `_assert_codes_match` (per TIGER_STYLE.md "verify
// type sizes and constant relationships before execution").
comptime {
    const std = @import("std");
    std.debug.assert(c.OVE_ERR_NOT_REGISTERED == -1);
    std.debug.assert(c.OVE_ERR_INVALID_PARAM == -2);
    std.debug.assert(c.OVE_ERR_NO_MEMORY == -3);
    std.debug.assert(c.OVE_ERR_TIMEOUT == -4);
    std.debug.assert(c.OVE_ERR_NOT_SUPPORTED == -5);
    std.debug.assert(c.OVE_ERR_QUEUE_FULL == -6);
    std.debug.assert(c.OVE_ERR_NET_REFUSED == -8);
    std.debug.assert(c.OVE_ERR_NET_UNREACHABLE == -9);
    std.debug.assert(c.OVE_ERR_NET_ADDR_IN_USE == -10);
    std.debug.assert(c.OVE_ERR_NET_RESET == -11);
    std.debug.assert(c.OVE_ERR_NET_DNS_FAIL == -12);
    std.debug.assert(c.OVE_ERR_NET_CLOSED == -13);
}
