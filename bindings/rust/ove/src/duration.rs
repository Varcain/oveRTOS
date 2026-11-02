// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Scalar timeout conversion independent of the optional clock API.

use core::time::Duration;

/// Convert a [`Duration`] to `u64` nanoseconds for the C ABI.
///
/// Saturates to `u64::MAX` if the duration overflows `u64` ns
/// (~584 years). Used internally by every binding wrapper that
/// passes a timeout to a substrate function taking `uint64_t timeout_ns`.
#[inline]
pub fn dur_to_ns(d: Duration) -> u64 {
    let n = d.as_nanos();
    if n > u64::MAX as u128 {
        u64::MAX
    } else {
        n as u64
    }
}
