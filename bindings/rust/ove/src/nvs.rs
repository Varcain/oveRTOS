// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Non-Volatile Storage (NVS) subsystem for oveRTOS.
//!
//! Provides key-value persistence backed by flash or EEPROM. Keys are
//! ordinary Rust strings; values remain arbitrary bytes. Keys are nonempty ASCII
//! `[A-Za-z0-9_.-]` without a leading dot. NUL, invalid grammar, or overlength
//! returns `InvalidParam` before storage access. [`key_max_len`] reflects the
//! selected RAM (63 bytes) or file backend (96 minus prefix, slash and NUL).

use core::ffi::CStr;

use crate::bindings;
use crate::cstring::Buffer;
use crate::error::{Error, Result};
use core::fmt;

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
pub fn read(key: &str, buf: &mut [u8]) -> Result<usize> {
    let mut out_len: usize = 0;
    let rc = unsafe {
        bindings::ove_nvs_read_n(
            key.as_ptr().cast(),
            key.len(),
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
pub fn write(key: &str, data: &[u8]) -> Result<()> {
    let rc = unsafe {
        bindings::ove_nvs_write_n(
            key.as_ptr().cast(),
            key.len(),
            data.as_ptr() as *const _,
            data.len(),
        )
    };
    Error::from_code(rc)
}

/// Erase the entry for `key` from NVS.
///
/// No-op when the key does not exist (the C API treats erase-of-missing
/// as success).
///
/// # Errors
/// Returns an error if the underlying storage backend reports a failure.
pub fn erase(key: &str) -> Result<()> {
    let rc = unsafe { bindings::ove_nvs_erase_n(key.as_ptr().cast(), key.len()) };
    Error::from_code(rc)
}

/// Maximum key length of the selected native backend, excluding NUL.
pub fn key_max_len() -> usize {
    unsafe { bindings::ove_nvs_key_max_len() }
}

/// Format a key once against the actual backend capacity, then write.
/// NUL, capacity or formatter failure is `InvalidParam`, with no partial key
/// publication and no allocator; native storage errors are propagated.
pub fn write_fmt(args: fmt::Arguments<'_>, data: &[u8]) -> Result<()> {
    let key = Buffer::<{ bindings::OVE_NVS_KEY_BUFFER_SIZE as usize }>::formatted(
        args,
        key_max_len(),
        Error::InvalidParam,
    )?;
    Error::from_code(unsafe {
        bindings::ove_nvs_write_n(
            key.as_ptr(),
            key.as_bytes().len(),
            data.as_ptr().cast(),
            data.len(),
        )
    })
}

/// Native C-string key compatibility; native validation remains authoritative.
pub fn read_cstr(key: &CStr, buf: &mut [u8]) -> Result<usize> {
    let mut len = 0;
    Error::from_code(unsafe {
        bindings::ove_nvs_read(key.as_ptr(), buf.as_mut_ptr().cast(), buf.len(), &mut len)
    })?;
    Ok(len)
}
/// Native C-string key compatibility.
pub fn write_cstr(key: &CStr, data: &[u8]) -> Result<()> {
    Error::from_code(unsafe {
        bindings::ove_nvs_write(key.as_ptr(), data.as_ptr().cast(), data.len())
    })
}
/// Native C-string key compatibility.
pub fn erase_cstr(key: &CStr) -> Result<()> {
    Error::from_code(unsafe { bindings::ove_nvs_erase(key.as_ptr()) })
}
