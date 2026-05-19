// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Board LED control — `set(led, on)` toggles a zero-based LED index.
//!
//! Wraps `ove/led.h`. LED indices and polarity are board-specific (see
//! `boards/<name>/board.yaml`). Available when `CONFIG_OVE_LED` is enabled.

const c = @import("c.zig").raw;

/// Set a board LED on or off by index.
///
/// `led` is a zero-based LED index. `on = true` turns the LED on;
/// `on = false` turns it off. LED indices and polarity are board-specific.
pub fn set(led: u32, on: bool) void {
    c.ove_led_set(led, if (on) @as(c_int, 1) else @as(c_int, 0));
}

/// Toggle the state of a board LED by index.
///
/// If the LED is on, it is turned off, and vice versa.
pub fn toggle(led: u32) void {
    c.ove_led_toggle(led);
}

/// Return the total number of LEDs available on the board.
pub fn count() u32 {
    return c.ove_led_count();
}
