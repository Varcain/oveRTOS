// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Async I2C bus wrapper with an `embedded_hal_async::i2c::I2c<SevenBitAddress>`
//! implementation when `embedded-hal-async` is enabled. Native seven-bit address
//! and independent TX/RX byte-count limits apply; requests are not split to
//! bypass provider limits. Submission has no timeout or automatic watchdog.
//! Native callbacks run in task context after provider retirement. This legacy
//! wrapper still requires stable owner/slot and application buffers through the
//! callback; cancellation/forget and shared per-future completion ownership are
//! not yet enforced by this wrapper.

use ::core::cell::UnsafeCell;
use ::core::ffi::c_void;
use ::core::future::poll_fn;
use ::core::task::Poll;

use crate::bindings;
use crate::error::{Error, Result};

use super::dma_slot::DmaSlot;

unsafe extern "C" fn dma_complete_cb(result: ::core::ffi::c_int, user_data: *mut c_void) {
    // The native callback requires this owner-held slot to stay at a stable
    // address through callback return; cancellation currently does not enforce it.
    let slot = unsafe { &*(user_data as *const DmaSlot) };
    slot.result_store(result as i32);
    slot.wake();
}

/// Async I2C bus master.
pub struct AsyncI2c {
    i2c: bindings::ove_i2c_t,
    slot: UnsafeCell<DmaSlot>,
}

// `AsyncI2c` wraps a native handle with serialized async admission. This legacy shared
// DmaSlot does not yet enforce distinct future result or backing ownership.
unsafe impl Send for AsyncI2c {}
unsafe impl Sync for AsyncI2c {}

impl AsyncI2c {
    /// Wrap an existing `ove_i2c_t` handle for async use.
    ///
    /// # Safety
    /// `handle` must be a valid I2C handle. Don't use the same handle
    /// through a sync [`crate::i2c::I2c`] wrapper while async
    /// transactions are in flight.
    #[inline]
    pub const unsafe fn from_handle(handle: bindings::ove_i2c_t) -> Self {
        Self {
            i2c: handle,
            slot: UnsafeCell::new(DmaSlot::new()),
        }
    }

    /// Async write-then-read with repeated start.
    pub async fn write_read(&self, addr: u16, tx: &[u8], rx: &mut [u8]) -> Result<()> {
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

        let slot: &DmaSlot = unsafe { &*self.slot.get() };
        slot.reset();

        let rc = unsafe {
            bindings::ove_i2c_write_read_async(
                self.i2c,
                addr,
                tx_ptr,
                tx.len(),
                rx_ptr,
                rx.len(),
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

    /// Async write-only transaction.
    pub async fn write(&self, addr: u16, data: &[u8]) -> Result<()> {
        let mut empty = [];
        self.write_read(addr, data, &mut empty).await
    }

    /// Async read-only transaction.
    pub async fn read(&self, addr: u16, buf: &mut [u8]) -> Result<()> {
        let empty: &[u8] = &[];
        self.write_read(addr, empty, buf).await
    }
}

#[cfg(feature = "embedded-hal-async")]
mod hal_async_impl {
    use super::AsyncI2c;
    use crate::error::Error;
    use embedded_hal::i2c::{ErrorType, Operation, SevenBitAddress};
    use embedded_hal_async::i2c::I2c;

    impl ErrorType for AsyncI2c {
        type Error = Error;
    }

    impl I2c<SevenBitAddress> for AsyncI2c {
        async fn transaction(
            &mut self,
            address: SevenBitAddress,
            operations: &mut [Operation<'_>],
        ) -> Result<(), Self::Error> {
            // Coalesce adjacent Write+Read pairs into a single
            // ove_i2c_write_read_async call; otherwise issue per
            // operation. This matches the embedded-hal-async semantics
            // (each op is a logical transfer; repeated start between).
            let mut i = 0;
            while i < operations.len() {
                match &mut operations[i..] {
                    [Operation::Write(w), Operation::Read(r), ..] => {
                        AsyncI2c::write_read(self, address as u16, w, r).await?;
                        i += 2;
                    }
                    [Operation::Write(w), ..] => {
                        AsyncI2c::write(self, address as u16, w).await?;
                        i += 1;
                    }
                    [Operation::Read(r), ..] => {
                        AsyncI2c::read(self, address as u16, r).await?;
                        i += 1;
                    }
                    [] => break,
                }
            }
            Ok(())
        }
    }
}
