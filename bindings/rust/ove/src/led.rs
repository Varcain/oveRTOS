// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! LED control for oveRTOS boards.
//!
//! LEDs are identified by zero-based index. Call [`count`] to discover how
//! many LEDs are available on the current board.

use crate::bindings;

/// Set the state of LED `led` (0-based index).
///
/// `on = true` turns the LED on, `on = false` turns it off.
pub fn set(led: u32, on: bool) {
    unsafe { bindings::ove_led_set(led, on as i32) }
}

/// Toggle the current state of LED `led` (on → off, off → on).
pub fn toggle(led: u32) {
    unsafe { bindings::ove_led_toggle(led) }
}

/// Return the number of LEDs available on this board.
pub fn count() -> u32 {
    unsafe { bindings::ove_led_count() }
}
