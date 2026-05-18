// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! `log::Log` backend for oveRTOS.
//!
//! Implements the [`log`] crate's facade ([`log::Log`]) on top of the
//! substrate's `ove_console_write` and the binding's [`FmtBuf`] stack
//! buffer.  Call [`init`] once at the start of `ove_main`, then use
//! `log::info!`/`log::warn!`/`log::error!`/etc. like in any other Rust
//! project.
//!
//! Output format matches the legacy `log_inf!`/`log_wrn!`/`log_err!`
//! macros (since-deleted), so a console log stream is byte-compatible
//! with the C `OVE_LOG_*` family:
//!
//! ```text
//! [I] message goes here
//! [W] warning goes here
//! [E] error goes here
//! ```
//!
//! Trace/Debug both render as `[D]` since the C side has no separate
//! trace prefix.
//!
//! # Buffer size and truncation
//!
//! Each [`log::Log::log`] call allocates a 256-byte stack buffer (no
//! heap dep).  Messages longer than ~250 bytes (after prefix + newline)
//! are silently truncated — the existing [`FmtBuf`] guarantees the
//! buffer remains null-terminated and well-formed.
//!
//! # Thread-safety
//!
//! `OveLogger` is `Sync` because every `Log::log` call owns a fresh
//! stack buffer — there is no shared mutable state.  The underlying
//! `ove_console_write` is the same primitive the C `OVE_LOG_*` macros
//! call, so per-line atomicity is governed by the backend (POSIX writes
//! to stderr atomically up to PIPE_BUF; FreeRTOS/Zephyr serialise via
//! their console mutex).

use crate::FmtBuf;
use crate::bindings;
use crate::error::{Error, Result};

/// `log::Log` implementation routing to `ove_console_write`.
///
/// Construct only via [`init`] — there's exactly one logger instance
/// per program (the standard `log` crate contract).
struct OveLogger;

impl log::Log for OveLogger {
    fn enabled(&self, _metadata: &log::Metadata<'_>) -> bool {
        // Filter is enforced by the `log` crate's global max-level
        // (`log::set_max_level`); per-record `enabled()` is left
        // permissive so users can override via `log::set_max_level`
        // without an extra round-trip through us.
        true
    }

    fn log(&self, record: &log::Record<'_>) {
        use core::fmt::Write;

        // 256 bytes matches the C `OVE_LOG_*` macros' stack buffer and
        // the legacy `log_inf!`/`log_wrn!`/`log_err!` shapes — keeps
        // console output byte-compatible across the four languages.
        let mut buf = [0u8; 256];
        let mut w = FmtBuf::new(&mut buf);

        let prefix: &str = match record.level() {
            log::Level::Error => "[E] ",
            log::Level::Warn => "[W] ",
            log::Level::Info => "[I] ",
            log::Level::Debug | log::Level::Trace => "[D] ",
        };

        // Truncation on overflow is fine — `FmtBuf` keeps the null
        // terminator and stops accepting new bytes.  No need to inspect
        // the write! Result.
        let _ = w.write_str(prefix);
        let _ = core::fmt::write(&mut w, *record.args());
        let _ = w.write_str("\n");

        // SAFETY: `as_bytes()` returns the buffer slice excluding the
        // null terminator; the substrate's `ove_console_write` takes a
        // (ptr, len) pair and does not retain the pointer.
        unsafe {
            bindings::ove_console_write(
                w.as_bytes().as_ptr() as *const _,
                w.as_bytes().len() as u32,
            );
        }
    }

    fn flush(&self) {
        // `ove_console_write` is synchronous; nothing to flush.
    }
}

static LOGGER: OveLogger = OveLogger;

/// Install the oveRTOS console logger as the global `log::Log` impl.
///
/// Call once near the top of `ove_main`.  After this returns, the
/// `log::info!`/`warn!`/`error!`/etc. family route through
/// `ove_console_write` exactly like the legacy custom log macros
/// (`log_inf`/`log_wrn`/`log_err`) used to before this iteration.
///
/// The global max level is set to [`log::LevelFilter::Info`] — apps
/// that want `debug`/`trace` output should call
/// `log::set_max_level(log::LevelFilter::Trace)` after this.
///
/// # Errors
/// Returns [`Error::InvalidParam`] if the global logger has already
/// been set (the `log` crate's `set_logger` is a one-shot).  Repeated
/// init is a programming error — apps that intentionally re-init
/// should use [`try_init`] and ignore the resulting error.
pub fn init() -> Result<()> {
    log::set_logger(&LOGGER).map_err(|_| Error::InvalidParam)?;
    log::set_max_level(log::LevelFilter::Info);
    Ok(())
}

/// Same as [`init`] but discards the "already-set" error.
///
/// Useful in test harnesses where setup may run multiple times.
pub fn try_init() {
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Info);
}
