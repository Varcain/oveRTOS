// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::ptr::{self, NonNull};
use core::sync::atomic::AtomicBool;
use core::task::Waker;

use crate::async_runtime::bus_state::{self, Bus};
use crate::bindings;
use crate::error::{Error, Result};
use crate::spi::{ChipSelect, Config, Word};

pub(super) use crate::async_runtime::bus_state::{Staging, close};
pub(super) type Header = bus_state::Header<Spi>;

pub(super) enum Spi {}

pub(super) struct Native {
    storage: UnsafeCell<MaybeUninit<bindings::ove_spi_storage_t>>,
    cs: UnsafeCell<MaybeUninit<bindings::ove_spi_cs>>,
    handle: bindings::ove_spi_t,
}

impl Bus for Spi {
    const NAME: &'static str = "SPI";
    type Native = Native;

    unsafe fn deinit(native: &Native) {
        if !native.handle.is_null() {
            unsafe { bindings::ove_spi_deinit(native.handle) };
        }
    }
    #[cfg(not(zero_heap))]
    unsafe fn alloc(size: usize, alignment: usize) -> *mut core::ffi::c_void {
        unsafe { bindings::ove_spi_binding_alloc(size, alignment) }
    }
    #[cfg(not(zero_heap))]
    unsafe fn free(pointer: *mut core::ffi::c_void) {
        unsafe { bindings::ove_spi_binding_free(pointer) };
    }
}

pub(super) unsafe fn initialize<W: Word>(
    header: NonNull<Header>,
    claim: *const AtomicBool,
    staging: Staging,
    config: Config,
) -> Result<()> {
    // SAFETY: allocation/claim gives exclusive uninitialized backing. The
    // native controller is written in place, without a stack temporary.
    unsafe {
        bus_state::initialize(header, claim, staging);
        let raw = ptr::addr_of_mut!((*header.as_ptr()).native);
        ptr::addr_of_mut!((*raw).storage).write(UnsafeCell::new(MaybeUninit::uninit()));
        ptr::addr_of_mut!((*raw).cs).write(UnsafeCell::new(MaybeUninit::uninit()));
        ptr::addr_of_mut!((*raw).handle).write(ptr::null_mut());
        let native = config.native::<W>();
        let mut handle = ptr::null_mut();
        let result = bindings::ove_spi_init(&mut handle, (*raw).storage.get().cast(), &native);
        if let Err(error) = Error::from_code(result) {
            // C already cleaned partial initialization; handle remains NULL
            // here, so final cleanup must not deinitialize the failed out pointer.
            bus_state::release(header);
            return Err(error);
        }
        (*raw).handle = handle;
    }
    Ok(())
}

pub(super) unsafe fn begin<W: Word>(
    header: NonNull<Header>,
    tx: *const W,
    tx_words: usize,
    words: usize,
    cs: Option<ChipSelect>,
    waiter: Waker,
) -> Result<usize> {
    unsafe {
        let bytes = words
            .checked_mul(size_of::<W>())
            .filter(|n| *n <= isize::MAX as usize)
            .ok_or(Error::InvalidParam)?;
        // SPI staging is symmetric: TX and RX both hold `words` words.
        let (previous, old_stage) = bus_state::check_idle(header, words, words)?;
        let handle = (*header.as_ptr()).native.handle;
        Error::from_code(bindings::ove_spi_binding_check_length(handle, bytes))?;
        let stage = bus_state::prepare::<Spi, W>(header, previous, old_stage, words, words)?;
        ptr::write_bytes(stage.tx, 0, bytes);
        ptr::write_bytes(stage.rx, 0, bytes);
        if tx_words != 0 {
            ptr::copy_nonoverlapping(tx, stage.tx.cast::<W>(), tx_words);
        }
        // CS has its own interior-mutable region. Metadata's short mutable
        // borrows must not alias a descriptor being read by native completion.
        let chip_select = match cs {
            Some(value) => {
                let target = (*header.as_ptr())
                    .native
                    .cs
                    .get()
                    .cast::<bindings::ove_spi_cs>();
                target.write(value.native());
                target as *const _
            }
            None => ptr::null(),
        };
        bus_state::submit(header, stage, waiter, |callback, user| {
            bindings::ove_spi_transfer_async(
                handle,
                chip_select,
                stage.tx.cast(),
                stage.rx.cast(),
                bytes,
                callback,
                user,
            )
        })
    }
}
