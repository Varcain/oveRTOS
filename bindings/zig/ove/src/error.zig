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
//! their owning modules and inherit from this set. The mapping uses the
//! `OVE_ERR_*` constants imported from the C header.

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
    /// Bus or controller busy: a transfer is already in flight, or the bus is
    /// held; retry later.
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
    /// A filesystem entry already exists.
    AlreadyExists,
    /// The storage device has no free space.
    NoSpace,
    /// A required path component is not a directory.
    NotDir,
    /// A file-only operation targeted a directory.
    IsDir,
    /// A directory is not empty.
    NotEmpty,
    /// The filesystem is read-only.
    ReadOnly,
    /// A storage input/output error occurred.
    Io,
    /// The resource is busy.
    Busy,
    /// A path or name is too long.
    NameTooLong,
    /// A handle is closed or invalid.
    BadHandle,
    /// Permission was denied.
    Permission,
    /// Paths belong to different filesystems.
    CrossDevice,
    /// The C layer returned a negative code this binding does not
    /// recognise — almost always because the substrate added a new
    /// `OVE_ERR_*` that `mapErrorCode` has not caught up with.  In
    /// `Debug` builds an unrecognised code panics at the FFI boundary
    /// (loud, localised drift detection); release builds surface it as
    /// this error instead of crashing a shipped binary.
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
        c.OVE_ERR_ALREADY_EXISTS => Error.AlreadyExists,
        c.OVE_ERR_NO_SPACE => Error.NoSpace,
        c.OVE_ERR_NOT_DIR => Error.NotDir,
        c.OVE_ERR_IS_DIR => Error.IsDir,
        c.OVE_ERR_NOT_EMPTY => Error.NotEmpty,
        c.OVE_ERR_READ_ONLY => Error.ReadOnly,
        c.OVE_ERR_IO => Error.Io,
        c.OVE_ERR_BUSY => Error.Busy,
        c.OVE_ERR_NAME_TOO_LONG => Error.NameTooLong,
        c.OVE_ERR_BAD_HANDLE => Error.BadHandle,
        c.OVE_ERR_PERMISSION => Error.Permission,
        c.OVE_ERR_CROSS_DEVICE => Error.CrossDevice,
        // An unrecognised substrate code is almost always a binding bug —
        // the substrate added a new `OVE_ERR_*` and this map didn't catch
        // up.  In `Debug` builds, panic at the FFI boundary to flag the
        // missing mapping during development and testing.
        // Release builds must not hard-crash a shipped device on an
        // unmapped code, so they degrade to `Error.UnknownErrorCode`.
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
