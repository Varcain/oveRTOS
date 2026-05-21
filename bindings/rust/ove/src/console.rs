// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Low-level console I/O for oveRTOS.
//!
//! Provides single-character read/write access to the platform console
//! (typically a UART). For formatted output, use the [`crate::log`] function
//! or the `log` crate facade installed by [`crate::log::init`].
//!
//! For **early-boot debug** before `log::init()` runs, or for one-off
//! prints from contexts where the `log` framework's mutexes aren't
//! reachable, use the [`crate::printk!`] / [`crate::ove_print!`] macros
//! which write directly via `ove_console_write` with a 256-byte
//! stack-formatted buffer. Same shape as Zephyr's `printk!`.

use crate::bindings;

/// Read a single character from the console.
///
/// Returns `Some(c)` where `c` is in `0..=255`, or `None` if no character
/// is currently available. The call may block until a character arrives
/// depending on the backend.
pub fn getchar() -> Option<i32> {
    let c = unsafe { bindings::ove_console_getchar() };
    if c < 0 { None } else { Some(c) }
}

/// Write a single character to the console output.
///
/// `c` is interpreted as an unsigned 8-bit byte (`0..=255`).
pub fn putchar(c: i32) {
    unsafe { bindings::ove_console_putchar(c) }
}

/// Write a byte slice to the console output (no formatting, no
/// newline). Backs the [`crate::printk!`] / [`crate::ove_print!`]
/// macros and is safe to call from any thread or ISR.
pub fn write_bytes(buf: &[u8]) {
    if buf.is_empty() {
        return;
    }
    // SAFETY: pointer + length cover a valid byte slice we own.
    unsafe {
        bindings::ove_console_write(buf.as_ptr().cast(), buf.len() as ::core::ffi::c_uint);
    }
}

/// Format arguments into a 256-byte stack buffer and write to the
/// console via [`write_bytes`]. Used by the [`crate::printk!`] and
/// [`crate::ove_print!`] macros; can also be called directly when a
/// pre-built [`core::fmt::Arguments`] is available.
///
/// Output is truncated silently if the formatted result exceeds the
/// buffer.
pub fn print_fmt(args: ::core::fmt::Arguments<'_>) {
    let mut buf = [0u8; 256];
    let mut w = crate::fmt::FmtBuf::new(&mut buf);
    let _ = ::core::fmt::Write::write_fmt(&mut w, args);
    write_bytes(w.as_bytes());
}

/// Format + write to the console without going through the `log`
/// framework. Useful for early-boot debug (before `log::init()`) and
/// for ISR-adjacent contexts.
///
/// Equivalent to Zephyr's `printk!`. Formats into a 256-byte stack
/// buffer and silently truncates if the result is larger.
///
/// ```ignore
/// ove::printk!("boot stage {}: {} bytes free\n", stage, free);
/// ```
#[macro_export]
macro_rules! printk {
    ($($arg:tt)*) => {
        $crate::console::print_fmt(::core::format_args!($($arg)*))
    };
}

/// Alias for [`printk!`] using the `ove_print!` name.
#[macro_export]
macro_rules! ove_print {
    ($($arg:tt)*) => {
        $crate::console::print_fmt(::core::format_args!($($arg)*))
    };
}

/// `printk!` variant that appends a newline.
#[macro_export]
macro_rules! printkln {
    () => { $crate::console::write_bytes(b"\n") };
    ($($arg:tt)*) => {
        $crate::console::print_fmt(::core::format_args!("{}\n", ::core::format_args!($($arg)*)))
    };
}
