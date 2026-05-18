// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! `embedded-io` 0.6 sync trait impls.
//!
//! Compiled only when the `embedded-io` Cargo feature is enabled.
//! Each section is further gated by the substrate `has_*` cfg for the
//! corresponding subsystem.  Provides byte-stream `Read` / `Write`
//! traits on `ove::Uart`, `ove::Stream`, and `ove::fs::File` so any
//! `embedded-io`-aware utility (line readers, codec drivers, simple
//! serial protocols) composes with oveRTOS I/O at zero per-call cost.
//!
//! Error mapping into [`embedded_io::ErrorKind`] follows the std::io
//! mirror: `Timeout` → `TimedOut`, `NoMemory` → `OutOfMemory`,
//! `NotFound` → `NotFound`, `InvalidParam` / `Inval` → `InvalidInput`,
//! `Eof` → `Other` (callers should not see this — see EOF semantics
//! note in the `Read` impls).

use crate::error::Error;

impl embedded_io::Error for Error {
    fn kind(&self) -> embedded_io::ErrorKind {
        match self {
            Error::Timeout => embedded_io::ErrorKind::TimedOut,
            Error::NoMemory => embedded_io::ErrorKind::OutOfMemory,
            Error::NotFound => embedded_io::ErrorKind::NotFound,
            Error::InvalidParam | Error::Inval => embedded_io::ErrorKind::InvalidInput,
            Error::NetRefused => embedded_io::ErrorKind::ConnectionRefused,
            Error::NetReset => embedded_io::ErrorKind::ConnectionReset,
            Error::NetClosed => embedded_io::ErrorKind::ConnectionAborted,
            Error::NotSupported => embedded_io::ErrorKind::Unsupported,
            _ => embedded_io::ErrorKind::Other,
        }
    }
}

// ---------------------------------------------------------------------------
// Uart
// ---------------------------------------------------------------------------

#[cfg(has_uart)]
mod uart_impl {
    use super::*;
    use crate::Uart;

    impl embedded_io::ErrorType for Uart {
        type Error = Error;
    }

    impl embedded_io::Read for Uart {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
            // Block effectively forever — `embedded_io::Read` has no
            // timeout in its contract; consumers wanting a bounded
            // read should call `Uart::read` directly with a duration.
            Uart::read(self, buf, core::time::Duration::from_secs(u64::MAX / 2))
        }
    }

    impl embedded_io::Write for Uart {
        fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
            Uart::write(self, buf, core::time::Duration::from_secs(u64::MAX / 2))
        }

        fn flush(&mut self) -> Result<(), Self::Error> {
            Uart::flush(self)
        }
    }
}

// ---------------------------------------------------------------------------
// Stream<N>
// ---------------------------------------------------------------------------

#[cfg(has_stream)]
mod stream_impl {
    use super::*;
    use crate::Stream;

    impl<const N: usize> embedded_io::ErrorType for Stream<N> {
        type Error = Error;
    }

    impl<const N: usize> embedded_io::Read for Stream<N> {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
            // Stream::recv blocks indefinitely until at least one
            // trigger-count of bytes is available — same forever-
            // blocking contract as embedded_io::Read.
            Stream::recv(self, buf)
        }
    }

    impl<const N: usize> embedded_io::Write for Stream<N> {
        fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
            Stream::send(self, buf)
        }

        fn flush(&mut self) -> Result<(), Self::Error> {
            // No explicit flush primitive — streams have no buffered
            // egress beyond the in-flight ring contents, and the
            // substrate writes synchronously.
            Ok(())
        }
    }
}

// ---------------------------------------------------------------------------
// fs::File
// ---------------------------------------------------------------------------

#[cfg(has_fs)]
mod file_impl {
    use super::*;
    use crate::fs::File;

    impl embedded_io::ErrorType for File {
        type Error = Error;
    }

    impl embedded_io::Read for File {
        fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
            // `Ok(0)` from `File::read` already signals EOF — the
            // substrate semantics match `embedded_io::Read` exactly,
            // so no translation needed.  `Error::Eof` shouldn't fire
            // here (substrate maps end-of-file to `Ok(0)`), but if
            // it ever does our `embedded_io::Error::kind()` impl
            // maps it to `ErrorKind::Other` — callers should rely on
            // `Ok(0)` for the EOF signal.
            File::read(self, buf)
        }
    }

    impl embedded_io::Write for File {
        fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
            File::write(self, buf)
        }

        fn flush(&mut self) -> Result<(), Self::Error> {
            // The substrate doesn't expose an explicit fsync/flush
            // primitive — writes go out synchronously and the FS
            // layer (LittleFS / Zephyr nvs-backed FS) handles its own
            // commit barriers.
            Ok(())
        }
    }
}
