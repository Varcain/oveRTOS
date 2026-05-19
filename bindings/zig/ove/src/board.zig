// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Board-level initialisation and identification.
//!
//! Wraps `ove/board.h`: `init()` runs the BSP bring-up (clocks, power rails,
//! peripherals) and must be called once at startup; `name()` returns the
//! null-terminated board name string set in the active board fragment.

const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

/// Initialize board-level hardware (clocks, power rails, peripherals).
///
/// Must be called once at startup before any peripheral APIs are used.
/// Returns `Error` if the board hardware fails to initialize.
pub fn init() Error!void {
    try err.fromCode(c.ove_board_init());
}

/// Return the null-terminated name string for this board (e.g. `"stm32f4_disco"`).
///
/// The returned pointer is valid for the lifetime of the program.
pub fn name() [*:0]const u8 {
    return c.ove_board_name();
}
