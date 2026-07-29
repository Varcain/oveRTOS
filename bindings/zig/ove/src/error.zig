// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Error set and FFI conversion helpers.
//!
//! `Error` is the binding's broad error union — every fallible `ove_*` C call
//! lowers an `int` return code into an `Error` via `fromCode(rc)`. Per-op
//! narrow sets (e.g. `Queue.SendError`, `Mutex.LockError`) are defined in
//! their owning modules and inherit from this set.

const std = @import("std");
const builtin = @import("builtin");
const c = @import("c.zig").raw;

/// Error set representing all possible oveRTOS failure codes.
///
/// Maps from the C-layer integer error codes to typed Zig errors.
pub const Error = error{
    /// The requested resource or command has not been registered.
    NotRegistered,
    /// An argument or configuration value is out of range or invalid.
    InvalidParam,
    /// Dynamic memory allocation failed (heap exhausted).  Named
    /// `OutOfMemory` to match `std.mem.Allocator.Error` so the
    /// allocator-returning `create(allocator)` paths in this binding
    /// compose naturally with stdlib `try`.
    OutOfMemory,
    /// A blocking operation timed out before completing.
    Timeout,
    /// The requested feature is not available on this platform or configuration.
    NotSupported,
    /// A message could not be enqueued because the queue is at capacity.
    QueueFull,
    /// ML inference or model loading failed.
    MlFailed,
    /// The remote end refused the connection.
    NetRefused,
    /// The destination network or host is unreachable.
    NetUnreachable,
    /// The requested local address is already in use.
    NetAddrInUse,
    /// The requested local address is not configured on this host.
    NetAddrNotAvailable,
    /// The connection was reset by the remote end.
    NetReset,
    /// DNS name resolution failed.
    NetDnsFail,
    /// The connection has been closed by the peer.
    NetClosed,
    /// Bus peripheral: device did not acknowledge.
    BusNack,
    /// Bus peripheral: bus is busy / arbitration lost.
    BusBusy,
    /// Bus peripheral: generic bus error.
    BusError,
    /// The queue was empty and no item could be received.
    QueueEmpty,
    /// A non-blocking operation would have had to block.
    WouldBlock,
    /// End of file / directory iterator exhausted.
    Eof,
    /// Argument or state is invalid for this operation.
    Inval,
    /// Requested key / entry / resource was not found.
    NotFound,
    /// The C layer returned a negative code this binding does not
    /// recognise — almost always because the substrate added a new
    /// `OVE_ERR_*` that `mapErrorCode` has not caught up with.  In
    /// `Debug` builds an unrecognised code panics at the FFI boundary
    /// (loud, localised drift detection); release builds surface it as
    /// this error instead of crashing a shipped binary — matching the
    /// graceful `Error::Unknown` policy of the Rust/C++ bindings.
    UnknownErrorCode,
};

/// Raw nanosecond sentinel meaning "block indefinitely" — the binding's
/// underlying value for `OVE_WAIT_FOREVER`.  Used internally by the
/// forever-blocking helpers; typed callers should prefer
/// [`time.Instant.forever`] for `*Until` deadline methods.
pub const wait_forever: u64 = c.OVE_WAIT_FOREVER;

inline fn mapErrorCode(rc: c_int) Error {
    return switch (rc) {
        c.OVE_ERR_NOT_REGISTERED => Error.NotRegistered,
        c.OVE_ERR_INVALID_PARAM => Error.InvalidParam,
        c.OVE_ERR_NO_MEMORY => Error.OutOfMemory,
        c.OVE_ERR_TIMEOUT => Error.Timeout,
        c.OVE_ERR_NOT_SUPPORTED => Error.NotSupported,
        c.OVE_ERR_QUEUE_FULL => Error.QueueFull,
        c.OVE_ERR_ML_FAILED => Error.MlFailed,
        c.OVE_ERR_NET_REFUSED => Error.NetRefused,
        c.OVE_ERR_NET_UNREACHABLE => Error.NetUnreachable,
        c.OVE_ERR_NET_ADDR_IN_USE => Error.NetAddrInUse,
        c.OVE_ERR_NET_ADDR_NOT_AVAILABLE => Error.NetAddrNotAvailable,
        c.OVE_ERR_NET_RESET => Error.NetReset,
        c.OVE_ERR_NET_DNS_FAIL => Error.NetDnsFail,
        c.OVE_ERR_NET_CLOSED => Error.NetClosed,
        c.OVE_ERR_BUS_NACK => Error.BusNack,
        c.OVE_ERR_BUS_BUSY => Error.BusBusy,
        c.OVE_ERR_BUS_ERROR => Error.BusError,
        c.OVE_ERR_QUEUE_EMPTY => Error.QueueEmpty,
        c.OVE_ERR_WOULD_BLOCK => Error.WouldBlock,
        c.OVE_ERR_EOF => Error.Eof,
        c.OVE_ERR_INVAL => Error.Inval,
        c.OVE_ERR_NOT_FOUND => Error.NotFound,
        // An unrecognised substrate code is almost always a binding bug —
        // the substrate added a new `OVE_ERR_*` and this map didn't catch
        // up.  In `Debug` builds, panic at the FFI boundary so the failure
        // is localised to the binding's pin set during development/tests.
        // Release builds must not hard-crash a shipped device on an
        // unmapped code, so they degrade to `Error.UnknownErrorCode`
        // (mirroring the Rust/C++ bindings' graceful `Error::Unknown`).
        else => blk: {
            if (builtin.mode == .Debug)
                std.debug.panic("ove binding: unrecognised C error code {d}", .{rc});
            break :blk Error.UnknownErrorCode;
        },
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
    std.debug.assert(c.OVE_ERR_NOT_REGISTERED == -1);
    std.debug.assert(c.OVE_ERR_INVALID_PARAM == -2);
    std.debug.assert(c.OVE_ERR_NO_MEMORY == -3);
    std.debug.assert(c.OVE_ERR_TIMEOUT == -4);
    std.debug.assert(c.OVE_ERR_NOT_SUPPORTED == -5);
    std.debug.assert(c.OVE_ERR_QUEUE_FULL == -6);
    std.debug.assert(c.OVE_ERR_ML_FAILED == -7);
    std.debug.assert(c.OVE_ERR_NET_REFUSED == -8);
    std.debug.assert(c.OVE_ERR_NET_UNREACHABLE == -9);
    std.debug.assert(c.OVE_ERR_NET_ADDR_IN_USE == -10);
    std.debug.assert(c.OVE_ERR_NET_RESET == -11);
    std.debug.assert(c.OVE_ERR_NET_DNS_FAIL == -12);
    std.debug.assert(c.OVE_ERR_NET_CLOSED == -13);
    std.debug.assert(c.OVE_ERR_BUS_NACK == -14);
    std.debug.assert(c.OVE_ERR_BUS_BUSY == -15);
    std.debug.assert(c.OVE_ERR_BUS_ERROR == -16);
    std.debug.assert(c.OVE_ERR_QUEUE_EMPTY == -17);
    std.debug.assert(c.OVE_ERR_WOULD_BLOCK == -18);
    std.debug.assert(c.OVE_ERR_EOF == -19);
    std.debug.assert(c.OVE_ERR_INVAL == -20);
    std.debug.assert(c.OVE_ERR_NOT_FOUND == -21);
    std.debug.assert(c.OVE_ERR_NET_ADDR_NOT_AVAILABLE == -22);
}
