// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS release version, from the C header `ove/version.h`.
//!
//! The header is the single source of truth.  `build.zig.zon` carries the
//! same number, which `make lint` checks; the firmware build compiles this
//! module without reading the zon, so nothing here can compare against it.
//! The exact sources a firmware image was built from are a separate matter,
//! the C `OVE_BUILD_OVERTOS_REV`.
//!
//! ```zig
//! if (ove.version.packed_version >= ove.version.number(0, 2, 0)) {
//!     // something 0.2.0 added
//! }
//! ```

const std = @import("std");
const c = @import("c.zig").raw;

/// Major version: incompatible API or ABI changes (`OVE_VERSION_MAJOR`).
pub const major: u32 = c.OVE_VERSION_MAJOR;
/// Minor version: compatible additions, 0..255 (`OVE_VERSION_MINOR`).
pub const minor: u32 = c.OVE_VERSION_MINOR;
/// Patch version: fixes, 0..255 (`OVE_VERSION_PATCH`).
pub const patch: u32 = c.OVE_VERSION_PATCH;
/// The version as one number for comparisons, packed as by `number`
/// (`OVE_VERSION`).
pub const packed_version: u32 = c.OVE_VERSION;
/// `"MAJOR.MINOR.PATCH"` (`OVE_VERSION_STRING`).
pub const string: []const u8 = c.OVE_VERSION_STRING;
/// The version as a `std.SemanticVersion`, for `order()` and formatting.
pub const semantic: std.SemanticVersion = .{ .major = major, .minor = minor, .patch = patch };

/// Packs a version the way `packed_version` is packed:
/// `(major << 16) | (minor << 8) | patch` (`OVE_VERSION_NUMBER`).
pub fn number(maj: u32, min: u32, pat: u32) u32 {
    return (maj << 16) | (min << 8) | pat;
}

comptime {
    std.debug.assert(packed_version == number(major, minor, patch));
    std.debug.assert(std.mem.eql(u8, string, std.fmt.comptimePrint("{d}.{d}.{d}", .{ major, minor, patch })));
}
