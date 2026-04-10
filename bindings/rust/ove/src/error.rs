// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS error types and the [`Result`] alias.

/// oveRTOS error codes, mapped from C `OVE_ERR_*` defines in `ove/types.h`.
///
/// When adding a new variant, update [`from_code`](Error::from_code),
/// [`to_code`](Error::to_code), [`Display`](core::fmt::Display), and
/// the `_assert_codes_match` function below.
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
    /// ML inference or model loading failed (`OVE_ERR_ML_FAILED`).
    MlFailed,
    /// Remote peer refused the connection (`OVE_ERR_NET_REFUSED`).
    NetRefused,
    /// Network or host is unreachable (`OVE_ERR_NET_UNREACHABLE`).
    NetUnreachable,
    /// Local address already in use (`OVE_ERR_NET_ADDR_IN_USE`).
    NetAddrInUse,
    /// Connection was reset by the remote peer (`OVE_ERR_NET_RESET`).
    NetReset,
    /// DNS name resolution failed (`OVE_ERR_NET_DNS_FAIL`).
    NetDnsFail,
    /// Connection closed by the remote peer (`OVE_ERR_NET_CLOSED`).
    NetClosed,
    /// Bus peripheral: device did not acknowledge (`OVE_ERR_BUS_NACK`).
    BusNack,
    /// Bus peripheral: bus is busy / arbitration lost (`OVE_ERR_BUS_BUSY`).
    BusBusy,
    /// Bus peripheral: generic bus error (`OVE_ERR_BUS_ERROR`).
    BusError,
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
            -7 => Err(Error::MlFailed),
            -8 => Err(Error::NetRefused),
            -9 => Err(Error::NetUnreachable),
            -10 => Err(Error::NetAddrInUse),
            -11 => Err(Error::NetReset),
            -12 => Err(Error::NetDnsFail),
            -13 => Err(Error::NetClosed),
            -14 => Err(Error::BusNack),
            -15 => Err(Error::BusBusy),
            -16 => Err(Error::BusError),
            other => Err(Error::Unknown(other)),
        }
    }

    /// Convert back to the raw C error code.
    pub fn to_code(self) -> i32 {
        match self {
            Error::NotRegistered => -1,
            Error::InvalidParam => -2,
            Error::NoMemory => -3,
            Error::Timeout => -4,
            Error::NotSupported => -5,
            Error::QueueFull => -6,
            Error::MlFailed => -7,
            Error::NetRefused => -8,
            Error::NetUnreachable => -9,
            Error::NetAddrInUse => -10,
            Error::NetReset => -11,
            Error::NetDnsFail => -12,
            Error::NetClosed => -13,
            Error::BusNack => -14,
            Error::BusBusy => -15,
            Error::BusError => -16,
            Error::Unknown(c) => c,
        }
    }

    /// Returns `true` if this is a networking-related error.
    pub fn is_net_error(&self) -> bool {
        matches!(
            self,
            Error::NetRefused
                | Error::NetUnreachable
                | Error::NetAddrInUse
                | Error::NetReset
                | Error::NetDnsFail
                | Error::NetClosed
        )
    }

    /// Returns `true` if this is a bus peripheral error (I2C/SPI).
    pub fn is_bus_error(&self) -> bool {
        matches!(self, Error::BusNack | Error::BusBusy | Error::BusError)
    }
}

// Compile-time assertion: verify Rust error codes match the C `OVE_ERR_*`
// defines from `ove/types.h`.  If the C header changes its numbering,
// bindgen will regenerate different constants and this function will fail
// to compile.
#[cfg(not(docsrs))]
const fn _assert_codes_match() {
    use crate::bindings;
    assert!(bindings::OVE_ERR_NOT_REGISTERED == -1);
    assert!(bindings::OVE_ERR_INVALID_PARAM == -2);
    assert!(bindings::OVE_ERR_NO_MEMORY == -3);
    assert!(bindings::OVE_ERR_TIMEOUT == -4);
    assert!(bindings::OVE_ERR_NOT_SUPPORTED == -5);
    assert!(bindings::OVE_ERR_QUEUE_FULL == -6);
    assert!(bindings::OVE_ERR_ML_FAILED == -7);
    assert!(bindings::OVE_ERR_NET_REFUSED == -8);
    assert!(bindings::OVE_ERR_NET_UNREACHABLE == -9);
    assert!(bindings::OVE_ERR_NET_ADDR_IN_USE == -10);
    assert!(bindings::OVE_ERR_NET_RESET == -11);
    assert!(bindings::OVE_ERR_NET_DNS_FAIL == -12);
    assert!(bindings::OVE_ERR_NET_CLOSED == -13);
    assert!(bindings::OVE_ERR_BUS_NACK == -14);
    assert!(bindings::OVE_ERR_BUS_BUSY == -15);
    assert!(bindings::OVE_ERR_BUS_ERROR == -16);
}

#[cfg(not(docsrs))]
const _: () = _assert_codes_match();

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Error::NotRegistered => write!(f, "not registered"),
            Error::InvalidParam => write!(f, "invalid parameter"),
            Error::NoMemory => write!(f, "out of memory"),
            Error::Timeout => write!(f, "timeout"),
            Error::NotSupported => write!(f, "not supported"),
            Error::QueueFull => write!(f, "queue full"),
            Error::MlFailed => write!(f, "ML inference failed"),
            Error::NetRefused => write!(f, "connection refused"),
            Error::NetUnreachable => write!(f, "network unreachable"),
            Error::NetAddrInUse => write!(f, "address in use"),
            Error::NetReset => write!(f, "connection reset"),
            Error::NetDnsFail => write!(f, "DNS resolution failed"),
            Error::NetClosed => write!(f, "connection closed"),
            Error::BusNack => write!(f, "bus NACK"),
            Error::BusBusy => write!(f, "bus busy"),
            Error::BusError => write!(f, "bus error"),
            Error::Unknown(c) => write!(f, "unknown error ({})", c),
        }
    }
}
