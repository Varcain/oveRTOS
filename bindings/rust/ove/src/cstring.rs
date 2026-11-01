// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Private, allocation-free staging for native bounded string consumers.

use crate::{Error, Result};
use core::ffi::c_char;
use core::fmt::{self, Write};

pub fn validate(bytes: &[u8]) -> Result<()> {
    if bytes.contains(&0) {
        Err(Error::InvalidParam)
    } else {
        Ok(())
    }
}

// Audio-only configurations need validation without bounded staging.
#[allow(dead_code)]
pub struct Buffer<const N: usize> {
    bytes: [u8; N],
    len: usize,
    limit: usize,
    overflow: Error,
    error: Option<Error>,
}

// Enabled consumers select different subsets of these staging operations.
#[allow(dead_code)]
impl<const N: usize> Buffer<N> {
    pub(super) fn from_bytes(bytes: &[u8]) -> Result<Self> {
        let mut buffer = Self::new(N.saturating_sub(1), Error::NameTooLong);
        buffer.write_str_bytes(bytes)?;
        Ok(buffer)
    }

    fn new(limit: usize, overflow: Error) -> Self {
        Self {
            bytes: [0; N],
            len: 0,
            limit: limit.min(N.saturating_sub(1)),
            overflow,
            error: None,
        }
    }

    pub(super) fn formatted(
        args: fmt::Arguments<'_>,
        limit: usize,
        overflow: Error,
    ) -> Result<Self> {
        let mut buffer = Self::new(limit, overflow);
        if fmt::write(&mut buffer, args).is_err() || buffer.error.is_some() {
            return Err(buffer.error.unwrap_or(Error::InvalidParam));
        }
        if N == 0 {
            return Err(overflow);
        }
        Ok(buffer)
    }

    fn write_str_bytes(&mut self, bytes: &[u8]) -> Result<()> {
        if let Some(error) = self.error {
            return Err(error);
        }
        let result = (|| {
            validate(bytes)?;
            if N == 0 || bytes.len() > self.limit - self.len {
                return Err(self.overflow);
            }
            self.bytes[self.len..self.len + bytes.len()].copy_from_slice(bytes);
            self.len += bytes.len();
            self.bytes[self.len] = 0;
            Ok(())
        })();
        if let Err(error) = result {
            self.error = Some(error);
        }
        result
    }

    pub(super) fn as_ptr(&self) -> *const c_char {
        self.bytes.as_ptr().cast()
    }
    pub(super) fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
}

impl<const N: usize> Write for Buffer<N> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        self.write_str_bytes(text.as_bytes())
            .map_err(|_| fmt::Error)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::cell::Cell;

    #[test]
    fn exact_bytes_non_utf8_and_terminator() {
        let value = Buffer::<5>::from_bytes(b"ab\xffd").unwrap();
        assert_eq!(value.as_bytes(), b"ab\xffd");
        assert_eq!(value.bytes[4], 0);
        assert!(matches!(
            Buffer::<5>::from_bytes(b"abcde"),
            Err(Error::NameTooLong)
        ));
        assert!(matches!(
            Buffer::<0>::from_bytes(b""),
            Err(Error::NameTooLong)
        ));
        assert_eq!(Buffer::<1>::from_bytes(b"").unwrap().bytes, [0]);
    }

    #[test]
    fn unicode_limit_is_bytes_and_nul_is_strict() {
        assert_eq!(
            Buffer::<5>::from_bytes("éé".as_bytes()).unwrap().as_bytes(),
            "éé".as_bytes()
        );
        assert!(matches!(
            Buffer::<4>::from_bytes("éé".as_bytes()),
            Err(Error::NameTooLong)
        ));
        for bytes in [b"\0abc".as_slice(), b"ab\0c", b"abc\0"] {
            assert!(matches!(
                Buffer::<8>::from_bytes(bytes),
                Err(Error::InvalidParam)
            ));
        }
    }

    #[test]
    fn formatting_is_once_and_errors_latch() {
        struct Once<'a>(&'a Cell<u8>);
        impl fmt::Display for Once<'_> {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.set(self.0.get() + 1);
                f.write_str("abcd")
            }
        }
        struct Swallow;
        impl fmt::Display for Swallow {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                let _ = f.write_str("too-long");
                let _ = f.write_str("x");
                Ok(())
            }
        }
        struct Fail;
        impl fmt::Display for Fail {
            fn fmt(&self, _: &mut fmt::Formatter<'_>) -> fmt::Result {
                Err(fmt::Error)
            }
        }
        let count = Cell::new(0);
        let value =
            Buffer::<5>::formatted(format_args!("{}", Once(&count)), 4, Error::NoSpace).unwrap();
        assert_eq!(count.get(), 1);
        assert_eq!(value.as_bytes(), b"abcd");
        assert!(matches!(
            Buffer::<5>::formatted(format_args!("{Swallow}"), 4, Error::NoSpace),
            Err(Error::NoSpace)
        ));
        assert!(matches!(
            Buffer::<5>::formatted(format_args!("{Fail}"), 4, Error::NoSpace),
            Err(Error::InvalidParam)
        ));
        assert!(matches!(
            Buffer::<5>::formatted(format_args!("a\0b"), 4, Error::NoSpace),
            Err(Error::InvalidParam)
        ));
    }
}
