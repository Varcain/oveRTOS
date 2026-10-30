// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Non-Volatile Storage (NVS) subsystem for oveRTOS.
//!
//! Provides key-value persistence backed by flash or EEPROM. Keys are C
//! strings, e.g. `c"my_key"`; values are bytes.

use core::ffi::CStr;

use crate::bindings;
use crate::error::{Error, Result};

// SAFETY (module-wide contract for the `unsafe { bindings::ove_*(...) }` FFI
// calls below): any handle passed to the C API is non-null and refers to a
// live RTOS object — wrapper constructors establish validity via
// `Error::from_code`, and `Drop` (or an explicit `deinit`) is the only place
// a handle is released. Pointer and slice arguments reference caller-owned
// memory valid for the duration of the call; the C side copies whatever it
// retains and does not alias them past return (verified against the
// signatures in `include/ove/*.h`). Blocks that deviate — `transmute`, raw
// pointer casts from user data, slice reconstruction via `from_raw_parts`,
// or storing a callback across the FFI boundary — carry their own
// `// SAFETY:` comment.

/// Initialize the NVS subsystem.
///
/// Must be called once before any `read`, `write`, or `erase` operations.
///
/// # Errors
/// Returns an error if the underlying storage backend fails to initialize.
pub fn init() -> Result<()> {
    let rc = unsafe { bindings::ove_nvs_init() };
    Error::from_code(rc)
}

/// Read the value stored under `key` into `buf`.
///
/// Returns the stored length of the value. When it exceeds `buf.len()` the
/// value was truncated to fit; only `buf.len()` bytes were copied.
///
/// # Errors
/// Returns [`Error::NotFound`] if the key does not exist, or another error
/// if the read fails.
pub fn read(key: &CStr, buf: &mut [u8]) -> Result<usize> {
    let mut out_len: usize = 0;
    let rc = unsafe {
        bindings::ove_nvs_read(
            key.as_ptr(),
            buf.as_mut_ptr() as *mut _,
            buf.len(),
            &mut out_len,
        )
    };
    Error::from_code(rc)?;
    Ok(out_len)
}

/// Write `data` under `key` in NVS.
///
/// If the key already exists its value is replaced.
///
/// # Errors
/// Returns [`Error::NoMemory`] if storage is full, or another error on failure.
pub fn write(key: &CStr, data: &[u8]) -> Result<()> {
    let rc =
        unsafe { bindings::ove_nvs_write(key.as_ptr(), data.as_ptr() as *const _, data.len()) };
    Error::from_code(rc)
}

/// Erase the entry for `key` from NVS.
///
/// No-op when the key does not exist (the C API treats erase-of-missing
/// as success).
///
/// # Errors
/// Returns an error if the underlying storage backend reports a failure.
pub fn erase(key: &CStr) -> Result<()> {
    let rc = unsafe { bindings::ove_nvs_erase(key.as_ptr()) };
    Error::from_code(rc)
}
