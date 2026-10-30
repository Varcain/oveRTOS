// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Stack-allocated formatting buffer for `no_std` environments.
//!
//! [`FmtBuf`] implements [`core::fmt::Write`] so you can use `write!` macros
//! without heap allocation, then pass the result to C APIs as a C string via
//! [`FmtBuf::as_c_str`] (or as a null-terminated byte slice via
//! [`FmtBuf::as_cstr`]).

use core::ffi::CStr;

use crate::error::{Error, Result};

/// Zero-allocation stack buffer that implements [`core::fmt::Write`].
///
/// One byte is always reserved for a null terminator so that [`as_cstr`](FmtBuf::as_cstr)
/// can return a C-compatible string without a mutable borrow.
pub struct FmtBuf<'a> {
    buf: &'a mut [u8],
    pos: usize,
    truncated: bool,
}

impl<'a> FmtBuf<'a> {
    /// Create a new `FmtBuf` backed by `buf`.
    ///
    /// The usable capacity is `buf.len() - 1` because one byte is reserved for
    /// the null terminator.
    pub fn new(buf: &'a mut [u8]) -> Self {
        if !buf.is_empty() {
            buf[0] = 0;
        }
        Self {
            buf,
            pos: 0,
            truncated: false,
        }
    }

    /// Returns the formatted content as a byte slice (no null terminator).
    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.pos]
    }

    /// Returns the formatted content as a null-terminated byte slice,
    /// suitable for passing to C APIs like LVGL.
    pub fn as_cstr(&self) -> &[u8] {
        // Null terminator is maintained by write_str after every write.
        &self.buf[..=self.pos]
    }

    /// Returns the formatted content as a C string, for APIs that take a
    /// `&CStr` (file paths, NVS keys, routes) built at run time.
    ///
    /// # Errors
    /// [`Error::InvalidParam`] if the content holds a NUL byte, or if it did
    /// not all fit: writes past `buf.len() - 1` bytes are dropped, and a
    /// silently shortened path or key would name something else.
    pub fn as_c_str(&self) -> Result<&CStr> {
        if self.truncated || self.buf.is_empty() {
            return Err(Error::InvalidParam);
        }
        CStr::from_bytes_with_nul(&self.buf[..=self.pos]).map_err(|_| Error::InvalidParam)
    }
}

impl core::fmt::Write for FmtBuf<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let bytes = s.as_bytes();
        let cap = self.buf.len().saturating_sub(1); // reserve 1 for null
        let avail = cap.saturating_sub(self.pos);
        let to_copy = bytes.len().min(avail);
        self.truncated |= to_copy < bytes.len();
        self.buf[self.pos..self.pos + to_copy].copy_from_slice(&bytes[..to_copy]);
        self.pos += to_copy;
        self.buf[self.pos] = 0; // maintain null invariant
        Ok(())
    }
}
