// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Allocator namespace for oveRTOS apps.
//!
//! `std.mem.Allocator` is an interface; the implementation behind it
//! determines whether memory comes from libc malloc (`c_allocator`),
//! the page allocator, or a static byte buffer (`FixedBufferAllocator`).
//! Zero-heap mode forbids the first two — substrate
//! `ove_heap_lock.c` panics if `malloc` runs after `ove.run()` engages
//! the gate, and `page_allocator` calls `mmap`/`VirtualAlloc` which
//! the substrate doesn't permit either.
//!
//! This namespace re-exports the safe (static-backed) allocators
//! verbatim, and provides `@compileError` shims for the dangerous ones
//! when `CONFIG_OVE_ZERO_HEAP=y`.  Users importing through
//! `ove.allocators.*` get a compile-time error with a clear remediation
//! message; users who bypass this namespace by importing `std.heap.*`
//! directly aren't caught here (B2-Layer-2 build-time symbol check
//! would catch those, B2-Layer-3 runtime gate the rest — both
//! deferred).

const std = @import("std");
const pin = @import("pin.zig");

/// `FixedBufferAllocator` — backed by a caller-provided byte slice.
/// The canonical zero-heap-compatible allocator.
pub const FixedBufferAllocator = std.heap.FixedBufferAllocator;

/// `ArenaAllocator` — bump-pointer over a backing allocator.  Safe in
/// zero-heap mode if the backing is static-backed (e.g. an FBA).
pub const ArenaAllocator = std.heap.ArenaAllocator;

/// `MemoryPool(T)` — fixed-size-block pool over a backing allocator.
/// Safe in zero-heap mode under the same condition as `ArenaAllocator`.
pub fn MemoryPool(comptime T: type) type {
    return std.heap.MemoryPool(T);
}

/// `StackFallbackAllocator(N, B)` — first tries an N-byte stack
/// buffer, falls back to backing allocator `B`.  Safe in zero-heap if
/// `B` is static-backed.
pub const StackFallbackAllocator = std.heap.StackFallbackAllocator;

// ---------------------------------------------------------------------------
// Banned in zero-heap mode (Layer 1 compile-time refusal).
//
// These allocators call into libc malloc / mmap / VirtualAlloc — the
// substrate's heap gate panics on any of them after `ove.run()`.  Users
// trying to import them in a zero-heap build hit `@compileError` at the
// import site with a remediation pointer.
// ---------------------------------------------------------------------------

/// libc malloc.  Banned in zero-heap mode.
pub const c_allocator = if (pin.zero_heap)
    @compileError(
        \\ove.allocators.c_allocator is unavailable in zero-heap builds.
        \\
        \\Zero-heap mode (CONFIG_OVE_ZERO_HEAP=y) forbids libc malloc/free
        \\after ove.run() engages the substrate's heap gate.  Use a
        \\static-backed allocator instead:
        \\
        \\    var arena_bytes: [4096]u8 = undefined;
        \\    var fba = std.heap.FixedBufferAllocator.init(&arena_bytes);
        \\    const allocator = fba.allocator();
        \\
        \\Or for typed pools:
        \\
        \\    var pool = ove.allocators.MemoryPool(MyT).init(fba.allocator());
        \\    defer pool.deinit();
    )
else
    std.heap.c_allocator;

/// mmap / VirtualAlloc.  Banned in zero-heap mode.
pub const page_allocator = if (pin.zero_heap)
    @compileError(
        \\ove.allocators.page_allocator is unavailable in zero-heap builds.
        \\
        \\Zero-heap mode forbids page-level allocations (mmap on POSIX,
        \\VirtualAlloc on Windows).  Use a FixedBufferAllocator over a
        \\static byte buffer instead.
    )
else
    std.heap.page_allocator;

/// Backed by `page_allocator` — banned in zero-heap mode.
pub fn GeneralPurposeAllocator(comptime config: anytype) type {
    if (pin.zero_heap) {
        @compileError(
            \\ove.allocators.GeneralPurposeAllocator is unavailable in zero-heap builds.
            \\
            \\GPA is backed by page_allocator under the hood; it would call
            \\mmap/VirtualAlloc which the substrate forbids.  Use a
            \\FixedBufferAllocator over a static byte buffer instead.
        );
    }
    return std.heap.GeneralPurposeAllocator(config);
}
