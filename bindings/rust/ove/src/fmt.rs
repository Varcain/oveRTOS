// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Stack-allocated formatting buffer for `no_std` environments.
//!
//! [`FmtBuf`] implements [`core::fmt::Write`] so you can use `write!` macros
//! without heap allocation, then pass the result to C APIs as a null-terminated
//! byte slice via [`FmtBuf::as_cstr`].

/// Zero-allocation stack buffer that implements [`core::fmt::Write`].
///
/// One byte is always reserved for a null terminator so that [`as_cstr`](FmtBuf::as_cstr)
/// can return a C-compatible string without a mutable borrow.
pub struct FmtBuf<'a> {
    buf: &'a mut [u8],
    pos: usize,
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
        Self { buf, pos: 0 }
    }

    /// Returns the formatted content as a byte slice (no null terminator).
    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.pos]
    }

    /// Returns the formatted content as a null-terminated byte slice,
    /// suitable for passing to C APIs like LVGL.
    pub fn as_cstr(&self) -> &[u8] {
        // Null terminator is maintained by write_str after every write.
        &self.buf[..self.pos + 1]
    }
}

impl core::fmt::Write for FmtBuf<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let bytes = s.as_bytes();
        let cap = self.buf.len().saturating_sub(1); // reserve 1 for null
        let avail = cap.saturating_sub(self.pos);
        let to_copy = bytes.len().min(avail);
        self.buf[self.pos..self.pos + to_copy].copy_from_slice(&bytes[..to_copy]);
        self.pos += to_copy;
        self.buf[self.pos] = 0; // maintain null invariant
        Ok(())
    }
}
