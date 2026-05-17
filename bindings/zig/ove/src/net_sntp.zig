// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! SNTP time synchronization client.
//!
//! Sends a single NTP query to a time server and stores the UTC offset
//! relative to the monotonic clock.  Useful for wall-clock timestamps,
//! TLS certificate validation, and log correlation.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// SNTP client configuration.
pub const Config = struct {
    /// NTP server hostname.
    server: [*:0]const u8 = "pool.ntp.org",
    /// Query timeout in nanoseconds (0 uses the default of 5 seconds).
    timeout_ns: u64 = 5 * std.time.ns_per_s,
};

/// Synchronize with an NTP server.
///
/// Sends a single NTP request and stores the computed UTC offset.
/// Subsequent calls update the stored offset.
pub fn sync(cfg: Config) Error!void {
    const c_cfg = c.ove_sntp_config_t{
        .server = cfg.server,
        .timeout_ns = cfg.timeout_ns,
    };
    try err.fromCode(c.ove_sntp_sync(&c_cfg));
}

/// Get the UTC offset computed by the last successful sync (microseconds).
///
/// The offset can be added to monotonic time to approximate wall-clock time.
pub fn getOffsetUs() Error!i64 {
    var offset: i64 = 0;
    try err.fromCode(c.ove_sntp_get_offset_us(&offset));
    return offset;
}

/// Get the current UTC time in seconds since Unix epoch.
///
/// Convenience function: returns monotonic time + NTP offset.
pub fn getUtc() Error!u32 {
    var utc: u32 = 0;
    try err.fromCode(c.ove_sntp_get_utc(&utc));
    return utc;
}
