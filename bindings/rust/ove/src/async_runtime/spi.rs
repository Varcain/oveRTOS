// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async SPI bus wrapper.
//!
//! Submits transfers via the C `ove_spi_transfer_async` API; the completion
//! callback wakes a registered `AtomicWaker`. Per-peripheral state in the
//! C storage struct enforces single-in-flight transfers — concurrent
//! submissions return [`Error::BusBusy`].
//!
//! With the `embedded-hal-async` Cargo feature the wrapper additionally
//! implements `embedded_hal_async::spi::SpiBus<u8>`, so any
//! `embedded-hal-async`-aware sensor driver works on top of it.

use ::core::cell::UnsafeCell;
use ::core::ffi::c_void;
use ::core::future::poll_fn;
use ::core::sync::atomic::{AtomicI32, Ordering};
use ::core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;

use crate::bindings;
use crate::error::{Error, Result};

/// Per-transfer slot — wakes the caller and stores the completion result.
/// `UnsafeCell` rather than `Mutex` because we use atomics for the
/// state field, and `AtomicWaker` is itself interior-mut-safe.
pub struct DmaSlot {
    waker: AtomicWaker,
    result: AtomicI32,
}

impl DmaSlot {
    /// Sentinel marking a transfer still in flight. `i32::MIN` is well
    /// outside the negative-OVE-error range (which is -1 to -20-ish).
    pub const PENDING: i32 = i32::MIN;

    pub const fn new() -> Self {
        Self {
            waker: AtomicWaker::new(),
            result: AtomicI32::new(Self::PENDING),
        }
    }

    pub fn reset(&self) {
        self.result.store(Self::PENDING, Ordering::Release);
    }

    pub fn result_store(&self, value: i32) {
        self.result.store(value, Ordering::Release);
    }

    pub fn result_load(&self) -> i32 {
        self.result.load(Ordering::Acquire)
    }

    pub fn register(&self, w: &::core::task::Waker) {
        self.waker.register(w);
    }

    pub fn wake(&self) {
        self.waker.wake();
    }
}

unsafe extern "C" fn dma_complete_cb(result: ::core::ffi::c_int, user_data: *mut c_void) {
    // SAFETY: user_data is a `*const DmaSlot` we passed in below; the
    // slot is owned by the future and outlives the callback because the
    // future awaits before dropping it.
    let slot = unsafe { &*(user_data as *const DmaSlot) };
    slot.result_store(result as i32);
    slot.wake();
}

/// Async SPI bus master.
pub struct AsyncSpi {
    spi: bindings::ove_spi_t,
    slot: UnsafeCell<DmaSlot>,
}

// SAFETY: the slot's atomic fields are interior-mutability-safe, and the
// C-side enforces single-in-flight via the async_busy flag. Multiple
// awaits on the same handle would race on the C-side and return
// OVE_ERR_BUS_BUSY rather than corrupting state.
unsafe impl Send for AsyncSpi {}
unsafe impl Sync for AsyncSpi {}

impl AsyncSpi {
    /// Wrap an existing `ove_spi_t` handle for async use.
    ///
    /// # Safety
    /// `handle` must be a valid SPI handle. The caller must not use the
    /// same handle through a sync [`crate::spi::Spi`] wrapper while async
    /// transfers are in flight.
    #[inline]
    pub const unsafe fn from_handle(handle: bindings::ove_spi_t) -> Self {
        Self {
            spi: handle,
            slot: UnsafeCell::new(DmaSlot::new()),
        }
    }

    /// Submit an async full-duplex transfer.
    ///
    /// `tx` and `rx` lengths must match the actual transfer size. The
    /// future is `'a`-bound to the buffers so they can't be dropped
    /// while in flight.
    pub async fn transfer<'a>(
        &'a self,
        cs: Option<&'a bindings::ove_spi_cs>,
        tx: &'a [u8],
        rx: &'a mut [u8],
    ) -> Result<()> {
        let len = tx.len().max(rx.len());
        if len == 0 {
            return Ok(());
        }
        let cs_ptr = cs.map_or(::core::ptr::null(), |c| c as *const _);
        let tx_ptr = if tx.is_empty() {
            ::core::ptr::null()
        } else {
            tx.as_ptr().cast()
        };
        let rx_ptr = if rx.is_empty() {
            ::core::ptr::null_mut()
        } else {
            rx.as_mut_ptr().cast()
        };

        // SAFETY: slot is owned by us and lives for 'a (until the future
        // is dropped). The C side stops accessing it once the callback
        // fires, and the .await below blocks until that happens.
        let slot: &DmaSlot = unsafe { &*self.slot.get() };
        slot.reset();

        let rc = unsafe {
            bindings::ove_spi_transfer_async(
                self.spi,
                cs_ptr,
                tx_ptr,
                rx_ptr,
                len,
                Some(dma_complete_cb),
                slot as *const _ as *mut c_void,
            )
        };
        Error::from_code(rc)?;

        poll_fn(|cx| {
            slot.register(cx.waker());
            let r = slot.result_load();
            if r == DmaSlot::PENDING {
                Poll::Pending
            } else {
                Poll::Ready(Error::from_code(r))
            }
        })
        .await
    }

    /// Async write-only transfer.
    pub async fn write<'a>(
        &'a self,
        cs: Option<&'a bindings::ove_spi_cs>,
        data: &'a [u8],
    ) -> Result<()> {
        let mut empty = [];
        self.transfer(cs, data, &mut empty).await
    }

    /// Async read-only transfer.
    pub async fn read<'a>(
        &'a self,
        cs: Option<&'a bindings::ove_spi_cs>,
        buf: &'a mut [u8],
    ) -> Result<()> {
        let empty: &[u8] = &[];
        self.transfer(cs, empty, buf).await
    }
}

#[cfg(feature = "embedded-hal-async")]
mod hal_async_impl {
    use super::AsyncSpi;
    use crate::error::Error;
    use embedded_hal::spi::ErrorType;
    use embedded_hal_async::spi::SpiBus;

    impl ErrorType for AsyncSpi {
        type Error = Error;
    }

    impl SpiBus<u8> for AsyncSpi {
        async fn read(&mut self, words: &mut [u8]) -> Result<(), Self::Error> {
            AsyncSpi::read(self, None, words).await
        }

        async fn write(&mut self, words: &[u8]) -> Result<(), Self::Error> {
            AsyncSpi::write(self, None, words).await
        }

        async fn transfer(&mut self, read: &mut [u8], write: &[u8]) -> Result<(), Self::Error> {
            // embedded_hal_async::spi::SpiBus::transfer allows different
            // tx/rx lengths; we pick the max and zero-extend the shorter
            // side via the underlying ove_spi_transfer_async semantics.
            AsyncSpi::transfer(self, None, write, read).await
        }

        async fn transfer_in_place(&mut self, words: &mut [u8]) -> Result<(), Self::Error> {
            // ove_spi_transfer_async doesn't natively support tx==rx;
            // copy through a stack buffer when small, otherwise iterate.
            let mut buf = [0u8; 64];
            let mut remaining = &mut words[..];
            while !remaining.is_empty() {
                let chunk = remaining.len().min(buf.len());
                buf[..chunk].copy_from_slice(&remaining[..chunk]);
                AsyncSpi::transfer(self, None, &buf[..chunk], &mut remaining[..chunk]).await?;
                let split = ::core::mem::take(&mut remaining);
                remaining = &mut split[chunk..];
            }
            Ok(())
        }

        async fn flush(&mut self) -> Result<(), Self::Error> {
            Ok(())
        }
    }
}
