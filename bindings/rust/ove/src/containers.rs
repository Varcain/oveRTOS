// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Fixed-capacity containers re-exported from [`heapless`].
//!
//! These containers compile in both heap and zeroheap modes because their
//! capacity is a const generic and their storage is owned in the type — no
//! global allocator is ever consulted.  Use them for general-purpose
//! collections (resizable vectors, fixed strings, hashmaps, deques) that
//! complement the RTOS-aware primitives (`ove::Queue`, `ove::Stream`).
//!
//! ```no_run
//! use ove::containers::{Vec, String};
//! use ove::containers::ContainerExt;
//!
//! let mut buf: Vec<u8, 64> = Vec::new();
//! let _ = buf.ove_push(0xAB);   // Result<(), ove::Error>
//!
//! let mut name: String<32> = String::new();
//! let _ = name.push_str("hello");
//! ```
//!
//! ## Choosing between `ove::heap::Vec` and `ove::containers::Vec`
//!
//! - [`crate::heap::Vec`] (re-exported from `alloc::vec::Vec`) is a
//!   growable vector backed by the global allocator.  Available only when
//!   the `alloc` (or `std`) feature is enabled and a `#[global_allocator]`
//!   is registered.  Cannot be used in zeroheap mode.
//!
//! - [`Vec<T, N>`](Vec) from this module is fixed-capacity; capacity is
//!   a const generic and storage lives inline in the type.  Always
//!   available, in both heap and zeroheap modes.  Fallible: `push`
//!   returns `Result<(), T>`; the [`ContainerExt::ove_push`] adapter
//!   converts that into [`crate::Error::NoMemory`] for consistency with
//!   the rest of the binding's error type.
//!
//! ## What is intentionally **not** re-exported
//!
//! - `heapless::spsc::Queue` and `heapless::mpmc::Q*` overlap semantically
//!   with [`crate::Queue`] (the kernel-aware FIFO).  Apps that explicitly
//!   want a lock-free SPSC ring should reach into `heapless::spsc` directly
//!   — the extra import documents the intent.

pub use heapless::{
    Deque, HistoryBuf, IndexMap, IndexSet, LinearMap, String, Vec, binary_heap, sorted_linked_list,
};

// `heapless::pool` is gated on architecture features (arm_llsc / 32- or
// 64-bit non-atomic-blocked targets); reach into `heapless::pool` directly
// from app code where supported, instead of relying on a re-export here
// that would fail to compile on hosts without those features.

use crate::{Error, Result};

/// Adapter trait that maps a [`heapless`] container's capacity-overflow
/// return into an [`crate::Error::NoMemory`].  Use the `ove_*` methods when
/// you want the result to flow through the same `?` chain as other
/// `ove::Result` calls.
///
/// The native `heapless` API is also available — pick whichever is clearer
/// at the call site.  The adapter discards the rejected element on
/// overflow; if you need to recover it, use the underlying `push` /
/// `push_back` directly.
pub trait ContainerExt<T> {
    /// Append `item`, returning [`crate::Error::NoMemory`] when full.
    fn ove_push(&mut self, item: T) -> Result<()>;
}

impl<T, const N: usize> ContainerExt<T> for Vec<T, N> {
    #[inline]
    fn ove_push(&mut self, item: T) -> Result<()> {
        self.push(item).map_err(|_| Error::NoMemory)
    }
}

impl<T, const N: usize> ContainerExt<T> for Deque<T, N> {
    #[inline]
    fn ove_push(&mut self, item: T) -> Result<()> {
        self.push_back(item).map_err(|_| Error::NoMemory)
    }
}

/// Append a string slice to a fixed-capacity [`String`], returning
/// [`crate::Error::NoMemory`] when the buffer is full.
#[inline]
pub fn ove_push_str<const N: usize>(s: &mut String<N>, value: &str) -> Result<()> {
    s.push_str(value).map_err(|_| Error::NoMemory)
}
