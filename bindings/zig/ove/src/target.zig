// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Typed compile-time target descriptors.
//!
//! Replaces ad-hoc `@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_*")` string
//! checks with a typed `Rtos` enum that callers `switch` over
//! exhaustively.  Adding a new RTOS makes every consuming switch
//! fail to compile until updated — string-based `@hasDecl` checks
//! would silently fall through.
//!
//! ```zig
//! const rtos_name: []const u8 = switch (ove.target.current_rtos) {
//!     .freertos => "FreeRTOS",
//!     .nuttx => "NuttX",
//!     .zephyr => "Zephyr",
//!     .posix => "POSIX",
//!     .wasm => "wasm",
//! };
//! ```

const c = @import("c.zig").raw;

/// Compile-time enumeration of the substrate's supported RTOSes.
pub const Rtos = enum {
    freertos,
    nuttx,
    zephyr,
    posix,
    wasm,
};

/// Comptime-resolved current RTOS from the substrate's
/// `CONFIG_OVE_RTOS_*` define.  Fails to compile if none is set —
/// adding a new RTOS requires extending both the enum and this
/// chain.
pub const current_rtos: Rtos =
    if (@hasDecl(c, "CONFIG_OVE_RTOS_FREERTOS")) .freertos else if (@hasDecl(c, "CONFIG_OVE_RTOS_NUTTX")) .nuttx else if (@hasDecl(c, "CONFIG_OVE_RTOS_ZEPHYR")) .zephyr else if (@hasDecl(c, "CONFIG_OVE_RTOS_POSIX")) .posix else if (@hasDecl(c, "CONFIG_OVE_RTOS_WASM")) .wasm else @compileError("ove.target: unknown RTOS — substrate must define one of CONFIG_OVE_RTOS_*");
