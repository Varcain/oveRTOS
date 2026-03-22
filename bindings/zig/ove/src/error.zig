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
    /// An unrecognized error code was returned by the C layer.
    Unknown,
};

/// Sentinel value for `timeout_ms` parameters meaning "block indefinitely".
pub const wait_forever: u32 = c.OVE_WAIT_FOREVER;

/// Convert a C error code to a Zig error or void.
pub fn fromCode(rc: c_int) Error!void {
    if (rc >= 0) return;
    return switch (rc) {
        c.OVE_ERR_NOT_REGISTERED => Error.NotRegistered,
        c.OVE_ERR_INVALID_PARAM => Error.InvalidParam,
        c.OVE_ERR_NO_MEMORY => Error.NoMemory,
        c.OVE_ERR_TIMEOUT => Error.Timeout,
        c.OVE_ERR_NOT_SUPPORTED => Error.NotSupported,
        c.OVE_ERR_QUEUE_FULL => Error.QueueFull,
        else => Error.Unknown,
    };
}
