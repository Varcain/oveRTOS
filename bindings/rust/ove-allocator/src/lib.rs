// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! libc-malloc-backed `#[global_allocator]` for oveRTOS no_std heap apps.
//!
//! This crate forwards Rust heap allocations to `malloc`, `free`, and
//! `realloc`. On FreeRTOS, oveRTOS wraps those calls to use the FreeRTOS
//! heap. NuttX and Zephyr use their libc malloc heaps. This lets `no_std`
//! heap-mode apps use `Box`, `Arc`, `Vec`, and `String`.
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
    use core::ffi::c_void;

    // Minimum alignment guaranteed by malloc on the supported targets.
    const MIN_ALIGN: usize = 2 * core::mem::size_of::<usize>();

    unsafe extern "C" {
        fn malloc(size: usize) -> *mut c_void;
        fn free(ptr: *mut c_void);
        fn realloc(ptr: *mut c_void, new_size: usize) -> *mut c_void;
    }

    /// A `GlobalAlloc` backed by libc `malloc`, `free`, and `realloc`.
    /// Over-aligned allocations keep the original `malloc` pointer in a header.
    pub struct OveAllocator;

    unsafe impl GlobalAlloc for OveAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            let align = layout.align();
            if align <= MIN_ALIGN {
                return unsafe { malloc(layout.size()) }.cast();
            }

            // Over-allocate because aligned_alloc bypasses the FreeRTOS malloc wrap.
            let Some(total) = layout.size().checked_add(align) else {
                return core::ptr::null_mut();
            };
            let raw: *mut u8 = unsafe { malloc(total) }.cast();
            if raw.is_null() {
                return raw;
            }

            // With MIN_ALIGN-aligned malloc, the header fits before the next
            // align boundary, at most align bytes from raw.
            let start = unsafe { raw.add(core::mem::size_of::<*mut u8>()) };
            let ptr = unsafe { start.add(start.align_offset(align)) };
            unsafe { ptr.cast::<*mut u8>().sub(1).write(raw) };
            ptr
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            let raw = if layout.align() <= MIN_ALIGN {
                ptr
            } else {
                unsafe { ptr.cast::<*mut u8>().sub(1).read() }
            };
            unsafe { free(raw.cast()) };
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            if layout.align() <= MIN_ALIGN {
                return unsafe { realloc(ptr.cast(), new_size) }.cast();
            }
            let new_layout = Layout::from_size_align(new_size, layout.align()).expect("layout");
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

    #[cfg(not(test))]
    #[global_allocator]
    static ALLOCATOR: OveAllocator = OveAllocator;

    #[cfg(test)]
    mod tests {
        use super::OveAllocator;
        use core::alloc::{GlobalAlloc, Layout};
        use core::slice::from_raw_parts;

        #[test]
        fn alloc_realloc_dealloc() {
            let data = b"oveRTOS allocation";
            for align in [8, 32, 64, 4096] {
                let initial = Layout::from_size_align(data.len(), align).unwrap();
                let grown = Layout::from_size_align(data.len() * 3, align).unwrap();
                let shrunk = Layout::from_size_align(data.len() / 2, align).unwrap();
                unsafe {
                    let ptr = OveAllocator.alloc(initial);
                    assert!(!ptr.is_null());
                    assert_eq!(ptr.addr() % align, 0);
                    ptr.copy_from_nonoverlapping(data.as_ptr(), data.len());

                    let ptr = OveAllocator.realloc(ptr, initial, grown.size());
                    assert!(!ptr.is_null());
                    assert_eq!(ptr.addr() % align, 0);
                    assert_eq!(from_raw_parts(ptr, data.len()), data);

                    let ptr = OveAllocator.realloc(ptr, grown, shrunk.size());
                    assert!(!ptr.is_null());
                    assert_eq!(ptr.addr() % align, 0);
                    assert_eq!(from_raw_parts(ptr, shrunk.size()), &data[..shrunk.size()]);
                    OveAllocator.dealloc(ptr, shrunk);
                }
            }
        }
    }
}

#[cfg(not(feature = "no_install"))]
pub use imp::OveAllocator;
