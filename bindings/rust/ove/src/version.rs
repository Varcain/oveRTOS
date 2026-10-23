// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS release version, from the C header `ove/version.h`.
//!
//! The header is the single source of truth; this crate's `Cargo.toml`
//! carries the same number, which `make lint` checks and this module also
//! checks when the crate compiles.  The exact sources a firmware image was
//! built from are a separate matter, the C `OVE_BUILD_OVERTOS_REV`.
//!
//! ```ignore
//! if ove::version::VERSION >= ove::version::number(0, 2, 0) {
//!     // something 0.2.0 added
//! }
//! ```

use crate::bindings;

/// Major version: incompatible API or ABI changes (`OVE_VERSION_MAJOR`).
pub const MAJOR: u32 = bindings::OVE_VERSION_MAJOR;
/// Minor version: compatible additions, 0..=255 (`OVE_VERSION_MINOR`).
pub const MINOR: u32 = bindings::OVE_VERSION_MINOR;
/// Patch version: fixes, 0..=255 (`OVE_VERSION_PATCH`).
pub const PATCH: u32 = bindings::OVE_VERSION_PATCH;
/// The version as one number for comparisons, packed as by [`number`]
/// (`OVE_VERSION`).
pub const VERSION: u32 = bindings::OVE_VERSION;
/// `"MAJOR.MINOR.PATCH"` (`OVE_VERSION_STRING`).
pub const VERSION_STR: &str = match bindings::OVE_VERSION_STRING.split_last() {
    // bindgen keeps the C string's terminating NUL.
    Some((0, s)) => match core::str::from_utf8(s) {
        Ok(s) => s,
        Err(_) => panic!("OVE_VERSION_STRING is not UTF-8"),
    },
    _ => panic!("OVE_VERSION_STRING is not NUL-terminated"),
};

/// Packs a version the way [`VERSION`] is packed:
/// `(major << 16) | (minor << 8) | patch` (`OVE_VERSION_NUMBER`).
#[must_use]
pub const fn number(major: u32, minor: u32, patch: u32) -> u32 {
    (major << 16) | (minor << 8) | patch
}

/// A decimal component of `CARGO_PKG_VERSION_*`.
const fn parse(s: &str) -> u32 {
    let b = s.as_bytes();
    assert!(!b.is_empty(), "empty version component");
    let mut n: u32 = 0;
    let mut i = 0;
    while i < b.len() {
        assert!(b[i].is_ascii_digit(), "version component is not a number");
        n = n * 10 + (b[i] - b'0') as u32;
        i += 1;
    }
    n
}

// This crate's Cargo.toml must carry the C header's version: a bump that
// misses one of them fails the build here as well as in `make lint`.
const _: () = {
    assert!(
        parse(env!("CARGO_PKG_VERSION_MAJOR")) == MAJOR
            && parse(env!("CARGO_PKG_VERSION_MINOR")) == MINOR
            && parse(env!("CARGO_PKG_VERSION_PATCH")) == PATCH,
        "bindings/rust/ove/Cargo.toml's version differs from include/ove/version.h \
         (or, in a docs.rs build, from the OVE_VERSION_* constants in bindings_stub.rs)"
    );
    assert!(VERSION == number(MAJOR, MINOR, PATCH));
};
