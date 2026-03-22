// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Low-level console I/O for oveRTOS.
//!
//! Provides single-character read/write access to the platform console
//! (typically a UART). For formatted output, use the [`crate::log`] function
//! or the [`crate::log_fmt!`] macro instead.

use crate::bindings;

/// Non-blocking read of a single character from the console.
///
/// Returns `Some(c)` where `c` is in `0..=127`, or `None` if no character
/// is currently available.
pub fn getchar() -> Option<i32> {
    let c = unsafe { bindings::ove_console_getchar() };
    if c < 0 { None } else { Some(c) }
}

/// Write a single character to the console output.
///
/// `c` should be a valid ASCII byte value (`0..=127`).
pub fn putchar(c: i32) {
    unsafe { bindings::ove_console_putchar(c) }
}
