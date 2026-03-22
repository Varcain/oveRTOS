// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS error types and the [`Result`] alias.

/// oveRTOS error codes, mapped from C `OVE_ERR_*` defines.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// The requested resource or subsystem has not been registered (`OVE_ERR_NOT_REGISTERED`).
    NotRegistered,
    /// One or more parameters passed to the API were invalid (`OVE_ERR_INVALID_PARAM`).
    InvalidParam,
    /// Heap or static allocation failed (`OVE_ERR_NO_MEMORY`).
    NoMemory,
    /// The operation did not complete within the allowed time (`OVE_ERR_TIMEOUT`).
    Timeout,
    /// The requested operation is not supported by this platform (`OVE_ERR_NOT_SUPPORTED`).
    NotSupported,
    /// The queue was full and the item could not be enqueued (`OVE_ERR_QUEUE_FULL`).
    QueueFull,
    /// An error code not covered by the above variants; the raw code is preserved.
    Unknown(i32),
}

/// Convenience alias for `core::result::Result<T, Error>`.
pub type Result<T> = core::result::Result<T, Error>;

/// Timeout value meaning "wait forever".
pub const WAIT_FOREVER: u32 = u32::MAX;

impl Error {
    /// Convert a C return code to `Result<()>`.
    /// Zero (OVE_OK) maps to `Ok(())`, negative values map to the
    /// corresponding `Error` variant.
    pub fn from_code(code: i32) -> Result<()> {
        match code {
            0 => Ok(()),
            -1 => Err(Error::NotRegistered),
            -2 => Err(Error::InvalidParam),
            -3 => Err(Error::NoMemory),
            -4 => Err(Error::Timeout),
            -5 => Err(Error::NotSupported),
            -6 => Err(Error::QueueFull),
            other => Err(Error::Unknown(other)),
        }
    }
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Error::NotRegistered => write!(f, "not registered"),
            Error::InvalidParam => write!(f, "invalid parameter"),
            Error::NoMemory => write!(f, "out of memory"),
            Error::Timeout => write!(f, "timeout"),
            Error::NotSupported => write!(f, "not supported"),
            Error::QueueFull => write!(f, "queue full"),
            Error::Unknown(c) => write!(f, "unknown error ({})", c),
        }
    }
}
