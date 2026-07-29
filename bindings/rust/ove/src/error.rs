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
///
/// The enum is `#[non_exhaustive]` so the substrate can add new
/// `OVE_ERR_*` codes (mapped to fresh variants here) without breaking
/// downstream `match` blocks at compile time.  Callers should always
/// include a `_ => …` arm.  Matches `std::io::ErrorKind` convention.
#[non_exhaustive]
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
    /// Requested local network address is not configured (`OVE_ERR_NET_ADDR_NOT_AVAILABLE`).
    NetAddrNotAvailable,
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
    /// The queue was empty and no item could be received (`OVE_ERR_QUEUE_EMPTY`).
    QueueEmpty,
    /// A non-blocking operation would have had to block (`OVE_ERR_WOULD_BLOCK`).
    WouldBlock,
    /// End of file / directory iterator exhausted (`OVE_ERR_EOF`).
    Eof,
    /// Argument or state is invalid for this operation (`OVE_ERR_INVAL`).
    Inval,
    /// Requested key / entry / resource was not found (`OVE_ERR_NOT_FOUND`).
    NotFound,
    /// A filesystem entry already exists (`OVE_ERR_ALREADY_EXISTS`).
    AlreadyExists,
    /// A storage device has no space remaining (`OVE_ERR_NO_SPACE`).
    NoSpace,
    /// A required path component is not a directory (`OVE_ERR_NOT_DIR`).
    NotDir,
    /// A file-only operation targeted a directory (`OVE_ERR_IS_DIR`).
    IsDir,
    /// A directory is not empty (`OVE_ERR_NOT_EMPTY`).
    NotEmpty,
    /// The target storage is read-only (`OVE_ERR_READ_ONLY`).
    ReadOnly,
    /// A storage input/output error occurred (`OVE_ERR_IO`).
    Io,
    /// The resource is busy (`OVE_ERR_BUSY`).
    Busy,
    /// A path or name is too long (`OVE_ERR_NAME_TOO_LONG`).
    NameTooLong,
    /// A handle is closed or invalid (`OVE_ERR_BAD_HANDLE`).
    BadHandle,
    /// Permission was denied (`OVE_ERR_PERMISSION`).
    Permission,
    /// Paths belong to different filesystems (`OVE_ERR_CROSS_DEVICE`).
    CrossDevice,
    /// An error code not covered by the above variants; the raw code is preserved.
    Unknown(i32),
}

/// Convenience alias for `core::result::Result<T, Error>`.
pub type Result<T> = core::result::Result<T, Error>;

impl Error {
    /// Convert a C return code to `Result<()>`.
    /// Zero (OVE_OK) maps to `Ok(())`, negative values map to the
    /// corresponding `Error` variant.
    #[inline]
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
            -17 => Err(Error::QueueEmpty),
            -18 => Err(Error::WouldBlock),
            -19 => Err(Error::Eof),
            -20 => Err(Error::Inval),
            -21 => Err(Error::NotFound),
            -22 => Err(Error::NetAddrNotAvailable),
            -23 => Err(Error::AlreadyExists),
            -24 => Err(Error::NoSpace),
            -25 => Err(Error::NotDir),
            -26 => Err(Error::IsDir),
            -27 => Err(Error::NotEmpty),
            -28 => Err(Error::ReadOnly),
            -29 => Err(Error::Io),
            -30 => Err(Error::Busy),
            -31 => Err(Error::NameTooLong),
            -32 => Err(Error::BadHandle),
            -33 => Err(Error::Permission),
            -34 => Err(Error::CrossDevice),
            other => Err(Error::Unknown(other)),
        }
    }

    /// Convert back to the raw C error code.
    #[inline]
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
            Error::QueueEmpty => -17,
            Error::WouldBlock => -18,
            Error::Eof => -19,
            Error::Inval => -20,
            Error::NotFound => -21,
            Error::NetAddrNotAvailable => -22,
            Error::AlreadyExists => -23,
            Error::NoSpace => -24,
            Error::NotDir => -25,
            Error::IsDir => -26,
            Error::NotEmpty => -27,
            Error::ReadOnly => -28,
            Error::Io => -29,
            Error::Busy => -30,
            Error::NameTooLong => -31,
            Error::BadHandle => -32,
            Error::Permission => -33,
            Error::CrossDevice => -34,
            Error::Unknown(c) => c,
        }
    }

    /// Returns `true` if this is a networking-related error.
    #[inline]
    pub fn is_net_error(&self) -> bool {
        matches!(
            self,
            Error::NetRefused
                | Error::NetUnreachable
                | Error::NetAddrInUse
                | Error::NetAddrNotAvailable
                | Error::NetReset
                | Error::NetDnsFail
                | Error::NetClosed
        )
    }

    /// Returns `true` if this is a bus peripheral error (I2C/SPI).
    #[inline]
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
    assert!(bindings::OVE_ERR_QUEUE_EMPTY == -17);
    assert!(bindings::OVE_ERR_WOULD_BLOCK == -18);
    assert!(bindings::OVE_ERR_EOF == -19);
    assert!(bindings::OVE_ERR_INVAL == -20);
    assert!(bindings::OVE_ERR_NOT_FOUND == -21);
    assert!(bindings::OVE_ERR_NET_ADDR_NOT_AVAILABLE == -22);
    assert!(bindings::OVE_ERR_ALREADY_EXISTS == -23);
    assert!(bindings::OVE_ERR_NO_SPACE == -24);
    assert!(bindings::OVE_ERR_NOT_DIR == -25);
    assert!(bindings::OVE_ERR_IS_DIR == -26);
    assert!(bindings::OVE_ERR_NOT_EMPTY == -27);
    assert!(bindings::OVE_ERR_READ_ONLY == -28);
    assert!(bindings::OVE_ERR_IO == -29);
    assert!(bindings::OVE_ERR_BUSY == -30);
    assert!(bindings::OVE_ERR_NAME_TOO_LONG == -31);
    assert!(bindings::OVE_ERR_BAD_HANDLE == -32);
    assert!(bindings::OVE_ERR_PERMISSION == -33);
    assert!(bindings::OVE_ERR_CROSS_DEVICE == -34);
}

#[cfg(not(docsrs))]
#[allow(clippy::used_underscore_items)] // intentional compile-time assertion
const _: () = _assert_codes_match();

/// `core::error::Error` is stable since Rust 1.81; our MSRV is 1.85 so
/// the impl can be unconditional.  `std::error::Error` is a re-export
/// of `core::error::Error` (also since 1.81), so this single `impl`
/// covers both `no_std` and `std` consumers — no `#[cfg(feature = "std")]`
/// gymnastics.  The default `source() -> None` is correct: `ove::Error`
/// doesn't chain to a lower-level cause.
impl core::error::Error for Error {}

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
            Error::QueueEmpty => write!(f, "queue empty"),
            Error::WouldBlock => write!(f, "would block"),
            Error::Eof => write!(f, "end of file"),
            Error::Inval => write!(f, "invalid argument"),
            Error::NotFound => write!(f, "not found"),
            Error::NetAddrNotAvailable => write!(f, "address not available"),
            Error::AlreadyExists => write!(f, "already exists"),
            Error::NoSpace => write!(f, "no space left"),
            Error::NotDir => write!(f, "not a directory"),
            Error::IsDir => write!(f, "is a directory"),
            Error::NotEmpty => write!(f, "directory not empty"),
            Error::ReadOnly => write!(f, "read-only filesystem"),
            Error::Io => write!(f, "I/O error"),
            Error::Busy => write!(f, "resource busy"),
            Error::NameTooLong => write!(f, "name too long"),
            Error::BadHandle => write!(f, "bad handle"),
            Error::Permission => write!(f, "permission denied"),
            Error::CrossDevice => write!(f, "cross-device operation"),
            Error::Unknown(c) => write!(f, "unknown error ({c})"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const KNOWN: &[(i32, Error)] = &[
        (-1, Error::NotRegistered),
        (-2, Error::InvalidParam),
        (-3, Error::NoMemory),
        (-4, Error::Timeout),
        (-5, Error::NotSupported),
        (-6, Error::QueueFull),
        (-7, Error::MlFailed),
        (-8, Error::NetRefused),
        (-9, Error::NetUnreachable),
        (-10, Error::NetAddrInUse),
        (-11, Error::NetReset),
        (-12, Error::NetDnsFail),
        (-13, Error::NetClosed),
        (-14, Error::BusNack),
        (-15, Error::BusBusy),
        (-16, Error::BusError),
        (-17, Error::QueueEmpty),
        (-18, Error::WouldBlock),
        (-19, Error::Eof),
        (-20, Error::Inval),
        (-21, Error::NotFound),
        (-22, Error::NetAddrNotAvailable),
        (-23, Error::AlreadyExists),
        (-24, Error::NoSpace),
        (-25, Error::NotDir),
        (-26, Error::IsDir),
        (-27, Error::NotEmpty),
        (-28, Error::ReadOnly),
        (-29, Error::Io),
        (-30, Error::Busy),
        (-31, Error::NameTooLong),
        (-32, Error::BadHandle),
        (-33, Error::Permission),
        (-34, Error::CrossDevice),
    ];

    #[test]
    fn from_code_zero_is_ok() {
        assert_eq!(Error::from_code(0), Ok(()));
    }

    #[test]
    fn from_code_known_codes_round_trip_through_to_code() {
        for &(code, expected) in KNOWN {
            assert_eq!(Error::from_code(code), Err(expected));
            assert_eq!(expected.to_code(), code);
        }
    }

    #[test]
    fn from_code_unknown_preserves_raw_code() {
        assert_eq!(Error::from_code(-999), Err(Error::Unknown(-999)));
        assert_eq!(Error::Unknown(-999).to_code(), -999);
        // Positive codes are also "Unknown" (the API contract is
        // negative-on-error, zero-on-ok; positive should not appear, but
        // preserving the raw code keeps round-tripping lossless).
        assert_eq!(Error::from_code(7), Err(Error::Unknown(7)));
    }

    #[test]
    fn classifier_predicates() {
        assert!(Error::NetRefused.is_net_error());
        assert!(Error::NetClosed.is_net_error());
        assert!(!Error::Timeout.is_net_error());
        assert!(!Error::Unknown(-99).is_net_error());

        assert!(Error::BusNack.is_bus_error());
        assert!(Error::BusError.is_bus_error());
        assert!(!Error::Timeout.is_bus_error());
        assert!(!Error::NetRefused.is_bus_error());
    }
}
