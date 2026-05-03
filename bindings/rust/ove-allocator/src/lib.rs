// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! libc-malloc-backed `#[global_allocator]` for oveRTOS no_std heap apps.
//!
//! All four oveRTOS RTOS backends (FreeRTOS, NuttX, Zephyr, POSIX) ship
//! a libc that exposes `malloc` / `free` / `realloc` and routes them to
//! the kernel heap.  This crate registers a `GlobalAlloc` shim that
//! forwards to those symbols, so a `no_std` heap-mode Rust binary can
//! use `Box`, `Arc`, `Vec`, `String`, etc. without any other allocator
//! crate.
//!
//! Usage from an app crate:
//! ```toml
//! [dependencies]
//! ove-allocator = { path = "../../../../bindings/rust/ove-allocator" }
//! ```
//! ```ignore
//! // Pull the crate in for its side-effect (the global_allocator static).
//! use ove_allocator as _;
//! ```
//!
//! On `std` builds where the standard library already provides an
//! allocator, enable the `no_install` feature so this crate becomes a
//! no-op:
//! ```toml
//! ove-allocator = { path = "...", default-features = false, features = ["no_install"] }
//! ```

#![no_std]

#[cfg(not(feature = "no_install"))]
mod imp {
    use core::alloc::{GlobalAlloc, Layout};

    unsafe extern "C" {
        fn malloc(size: usize) -> *mut core::ffi::c_void;
        fn free(ptr: *mut core::ffi::c_void);
        fn realloc(ptr: *mut core::ffi::c_void, new_size: usize) -> *mut core::ffi::c_void;
        fn aligned_alloc(alignment: usize, size: usize) -> *mut core::ffi::c_void;
    }

    /// `GlobalAlloc` impl that forwards to libc malloc/free/realloc.
    /// Alignments stricter than `align_of::<usize>()` go through
    /// `aligned_alloc` (C11; available on all four supported backends).
    pub struct OveAllocator;

    unsafe impl GlobalAlloc for OveAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            // libc malloc() is documented as returning storage suitable
            // for any built-in alignment (`max_align_t`).  Anything
            // stricter — SIMD vectors, page boundaries — goes through
            // aligned_alloc.
            let ptr = if layout.align() <= core::mem::align_of::<usize>() {
                unsafe { malloc(layout.size()) }
            } else {
                // C11 aligned_alloc requires size to be a multiple of
                // alignment; round up.
                let size = (layout.size() + layout.align() - 1) & !(layout.align() - 1);
                unsafe { aligned_alloc(layout.align(), size) }
            };
            ptr as *mut u8
        }

        unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
            unsafe { free(ptr as *mut _) };
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            // Only safe to use libc realloc when the alignment fits the
            // built-in max_align_t; otherwise fall back to alloc/copy.
            if layout.align() <= core::mem::align_of::<usize>() {
                unsafe { realloc(ptr as *mut _, new_size) as *mut u8 }
            } else {
                let new_layout =
                    Layout::from_size_align(new_size, layout.align()).expect("layout");
                let new_ptr = unsafe { self.alloc(new_layout) };
                if !new_ptr.is_null() {
                    let copy = core::cmp::min(layout.size(), new_size);
                    unsafe {
                        core::ptr::copy_nonoverlapping(ptr, new_ptr, copy);
                        self.dealloc(ptr, layout);
                    }
                }
                new_ptr
            }
        }
    }

    #[global_allocator]
    static ALLOCATOR: OveAllocator = OveAllocator;
}

#[cfg(not(feature = "no_install"))]
pub use imp::OveAllocator;
