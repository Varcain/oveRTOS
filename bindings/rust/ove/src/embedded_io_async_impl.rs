// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! `embedded-io-async` 0.7 trait impls.
//!
//! Compiled when both the `embedded-io-async` and `async` Cargo
//! features are enabled. Provides `Read` on the async wrappers
//! ([`AsyncUart`], [`AsyncStream`]) so any `embedded-io-async`-aware
//! crate (line readers, codec drivers, async-MQTT clients) composes
//! with oveRTOS I/O.
//!
//! `Read::read` takes `&mut self`; our wrappers have `&'static self`
//! receivers. The impl is therefore on `&'static T` — `&mut &'static T`
//! satisfies the trait's `&mut self` signature without imposing
//! exterior mutability on the wrapper itself.
//!
//! Error mapping shares the sync `embedded_io_impl` via the
//! `ErrorType` trait, which `embedded_io_async::Read` extends.

use crate::error::Error;

// ---------------------------------------------------------------------------
// AsyncUart
// ---------------------------------------------------------------------------

#[cfg(has_uart)]
mod uart_impl {
    use super::*;
    use crate::async_runtime::AsyncUart;

    impl embedded_io::ErrorType for &'static AsyncUart {
        type Error = Error;
    }

    impl embedded_io_async::Read for &'static AsyncUart {
        async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
            AsyncUart::read(*self, buf).await
        }
    }
}

// ---------------------------------------------------------------------------
// AsyncStream<N>
// ---------------------------------------------------------------------------

#[cfg(has_stream)]
mod stream_impl {
    use super::*;
    use crate::async_runtime::AsyncStream;

    impl<const N: usize> embedded_io::ErrorType for &'static AsyncStream<N> {
        type Error = Error;
    }

    impl<const N: usize> embedded_io_async::Read for &'static AsyncStream<N> {
        async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
            AsyncStream::read(*self, buf).await
        }
    }
}
