// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Blocking SNTP time synchronization client.
//!
//! Provides a safe Rust API for the oveRTOS SNTP subsystem.  A single NTP
//! query is sent to a time server and the resulting UTC offset is stored
//! internally.  Useful for wall-clock timestamps, TLS certificate
//! validation, and log correlation.
//!
//! ## Async alternative
//!
//! For async SNTP on top of [`crate::async_net`] use the
//! [`sntpc`](https://crates.io/crates/sntpc) crate from crates.io
//! with the `embassy-socket` feature. Single-shot NTP query against
//! any UDP-reachable server; no global UTC-offset state.
//!
//! # Example
//!
//! ```ignore
//! use ove::net_sntp;
//!
//! let cfg = net_sntp::Config {
//!     server: b"pool.ntp.org\0",
//!     timeout: core::time::Duration::from_secs(5),
//! };
//! net_sntp::sync(&cfg).unwrap();
//! let utc = net_sntp::get_utc().unwrap();
//! ```

use crate::bindings;
use crate::error::{Error, Result};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

/// SNTP client configuration.
///
/// `server` must be a null-terminated byte string (e.g. `b"pool.ntp.org\0"`).
/// A `timeout` of `Duration::ZERO` uses the default (5 s).
pub struct Config<'a> {
    /// NTP server hostname (null-terminated).
    pub server: &'a [u8],
    /// Query timeout (`Duration::ZERO` selects the default of 5 s).
    pub timeout: core::time::Duration,
}

impl Default for Config<'_> {
    fn default() -> Self {
        Self {
            server: b"pool.ntp.org\0",
            timeout: core::time::Duration::from_secs(5),
        }
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Synchronize with an NTP server.
///
/// Sends a single NTP request and stores the computed UTC offset.
/// Subsequent calls update the stored offset.
///
/// # Errors
/// Returns an error if the NTP query fails.
pub fn sync(cfg: &Config) -> Result<()> {
    let c_cfg = bindings::ove_sntp_config_t {
        server: cfg.server.as_ptr() as *const _,
        timeout_ns: crate::time::dur_to_ns(cfg.timeout),
    };
    let rc = unsafe { bindings::ove_sntp_sync(&c_cfg) };
    Error::from_code(rc)
}

/// Get the UTC offset computed by the last successful sync.
///
/// The offset can be added to `ove_time_get_us()` to approximate
/// wall-clock time (microseconds since Unix epoch).
///
/// # Errors
/// Returns `Error::NotSupported` if no sync has been performed.
pub fn get_offset_us() -> Result<i64> {
    let mut offset: i64 = 0;
    let rc = unsafe { bindings::ove_sntp_get_offset_us(&mut offset) };
    Error::from_code(rc)?;
    Ok(offset)
}

/// Get the current UTC time in seconds since Unix epoch.
///
/// Convenience function: returns monotonic time + NTP offset.
///
/// # Errors
/// Returns `Error::NotSupported` if no sync has been performed.
pub fn get_utc() -> Result<u32> {
    let mut utc: u32 = 0;
    let rc = unsafe { bindings::ove_sntp_get_utc(&mut utc) };
    Error::from_code(rc)?;
    Ok(utc)
}
