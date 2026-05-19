// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Backward-compatibility BSP shim — re-exports `board`, `gpio`, `led`.
//!
//! Provided for API parity with the C `<ove/bsp.h>` umbrella header. New
//! code should prefer the per-subsystem modules directly.

/// Board initialization and identification (re-exported from board.zig).
pub const board = @import("board.zig");
/// GPIO pin configuration and interrupt registration (re-exported from gpio.zig).
pub const gpio = @import("gpio.zig");
/// LED on/off/toggle control by index (re-exported from led.zig).
pub const led = @import("led.zig");
