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
use crate::i2c::Config;

pub(super) use crate::async_runtime::bus_state::{Staging, close};
pub(super) type Header = bus_state::Header<I2c>;

pub(super) enum I2c {}

pub(super) struct Native {
    storage: UnsafeCell<MaybeUninit<bindings::ove_i2c_storage_t>>,
    handle: bindings::ove_i2c_t,
}

impl Bus for I2c {
    const NAME: &'static str = "I2C";
    type Native = Native;

    unsafe fn deinit(native: &Native) {
        if !native.handle.is_null() {
            unsafe { bindings::ove_i2c_deinit(native.handle) };
        }
    }
    #[cfg(not(zero_heap))]
    unsafe fn alloc(size: usize, alignment: usize) -> *mut core::ffi::c_void {
        unsafe { bindings::ove_i2c_binding_alloc(size, alignment) }
    }
    #[cfg(not(zero_heap))]
    unsafe fn free(pointer: *mut core::ffi::c_void) {
        unsafe { bindings::ove_i2c_binding_free(pointer) };
    }
}

pub(super) unsafe fn initialize(
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
        ptr::addr_of_mut!((*raw).handle).write(ptr::null_mut());
        let native = config.native();
        let mut handle = ptr::null_mut();
        let result = bindings::ove_i2c_init(&mut handle, (*raw).storage.get().cast(), &native);
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

pub(super) unsafe fn begin(
    header: NonNull<Header>,
    address: u16,
    tx: *const u8,
    tx_len: usize,
    rx_len: usize,
    waiter: Waker,
) -> Result<usize> {
    unsafe {
        if tx_len > isize::MAX as usize || rx_len > isize::MAX as usize {
            return Err(Error::InvalidParam);
        }
        let handle = (*header.as_ptr()).native.handle;
        // Pure scalar/provider preflight precedes allocation and payload copies.
        Error::from_code(bindings::ove_i2c_binding_check_lengths(
            handle, address, tx_len, rx_len,
        ))?;
        let (previous, old_stage) = bus_state::check_idle(header, tx_len, rx_len)?;
        let stage = bus_state::prepare::<I2c, u8>(header, previous, old_stage, tx_len, rx_len)?;
        if tx_len != 0 {
            ptr::copy_nonoverlapping(tx, stage.tx, tx_len);
        }
        if rx_len != 0 {
            ptr::write_bytes(stage.rx, 0, rx_len);
        }
        bus_state::submit(header, stage, waiter, |callback, user| {
            bindings::ove_i2c_write_read_async(
                handle,
                address,
                if tx_len == 0 {
                    ptr::null()
                } else {
                    stage.tx.cast()
                },
                tx_len,
                if rx_len == 0 {
                    ptr::null_mut()
                } else {
                    stage.rx.cast()
                },
                rx_len,
                callback,
                user,
            )
        })
    }
}
