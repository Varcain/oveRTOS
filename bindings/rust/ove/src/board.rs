// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Board initialization and identification for oveRTOS.
//!
//! Provides [`init`] to bring up all board peripherals and [`name`] to retrieve
//! the board identifier string at runtime.

use crate::bindings;
use crate::error::{Error, Result};

/// Initialize the board (clocks, pinmux, and core peripherals).
///
/// Must be called once at startup before using any peripheral APIs.
///
/// # Errors
/// Returns an error if any hardware peripheral fails to initialize.
pub fn init() -> Result<()> {
    let rc = unsafe { bindings::ove_board_init() };
    Error::from_code(rc)
}

/// Return the board name as a static string (e.g. `"stm32f4-disco"`).
///
/// Returns `"unknown"` if the board name is unavailable or not valid UTF-8.
pub fn name() -> &'static str {
    let ptr = unsafe { bindings::ove_board_name() };
    if ptr.is_null() {
        "unknown"
    } else {
        unsafe { core::ffi::CStr::from_ptr(ptr) }
            .to_str()
            .unwrap_or("unknown")
    }
}
