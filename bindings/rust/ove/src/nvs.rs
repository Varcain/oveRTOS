// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Non-Volatile Storage (NVS) subsystem for oveRTOS.
//!
//! Provides key-value persistence backed by flash or EEPROM. All keys must be
//! null-terminated byte slices (e.g. `b"my_key\0"`).

use crate::bindings;
use crate::error::{Error, Result};

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

/// Read a value from NVS into `buf`. `key` must be `\0`-terminated.
///
/// Returns the number of bytes actually read, which may be less than `buf.len()`
/// if the stored value is shorter.
///
/// # Errors
/// Returns [`Error::NotRegistered`] if the key does not exist, or another error
/// if the read fails.
pub fn read(key: &[u8], buf: &mut [u8]) -> Result<usize> {
    let mut out_len: usize = 0;
    let rc = unsafe {
        bindings::ove_nvs_read(
            key.as_ptr() as *const _,
            buf.as_mut_ptr() as *mut _,
            buf.len(),
            &mut out_len,
        )
    };
    Error::from_code(rc)?;
    Ok(out_len)
}

/// Write `data` under `key` in NVS. `key` must be `\0`-terminated.
///
/// If the key already exists its value is replaced.
///
/// # Errors
/// Returns [`Error::NoMemory`] if storage is full, or another error on failure.
pub fn write(key: &[u8], data: &[u8]) -> Result<()> {
    let rc = unsafe {
        bindings::ove_nvs_write(
            key.as_ptr() as *const _,
            data.as_ptr() as *const _,
            data.len(),
        )
    };
    Error::from_code(rc)
}

/// Erase the entry for `key` from NVS. `key` must be `\0`-terminated.
///
/// # Errors
/// Returns [`Error::NotRegistered`] if the key does not exist.
pub fn erase(key: &[u8]) -> Result<()> {
    let rc = unsafe { bindings::ove_nvs_erase(key.as_ptr() as *const _) };
    Error::from_code(rc)
}
