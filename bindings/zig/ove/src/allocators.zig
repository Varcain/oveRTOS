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
///
/// Returns a `std.mem.Allocator` that calls `extern "C" malloc / free /
/// realloc` directly.  Unlike `std.heap.c_allocator`, we don't require
/// `builtin.link_libc` to be true — Zig sees a binding library as
/// `freestanding` since libc is linked at the C-level (picolibc / glibc),
/// not via `zig build`.  Under heap-mode FreeRTOS those `malloc` /
/// `free` / `realloc` symbols are wrapped by
/// `backends/freertos/freertos_libc_malloc.c` so every libc malloc
/// transparently routes through `pvPortMalloc` (single heap policy).
pub const c_allocator: std.mem.Allocator = if (pin.zero_heap)
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
    libc_allocator_impl.allocator();

const libc_allocator_impl = struct {
    extern fn malloc(usize) ?*anyopaque;
    extern fn free(?*anyopaque) void;
    extern fn realloc(?*anyopaque, usize) ?*anyopaque;

    const vtable: std.mem.Allocator.VTable = .{
        .alloc = allocFn,
        .resize = resizeFn,
        .remap = remapFn,
        .free = freeFn,
    };

    fn allocator() std.mem.Allocator {
        return .{ .ptr = undefined, .vtable = &vtable };
    }

    fn allocFn(
        _: *anyopaque,
        len: usize,
        alignment: std.mem.Alignment,
        _: usize,
    ) ?[*]u8 {
        // libc malloc returns 8-byte (or stricter) aligned memory; if
        // the caller wants tighter alignment we'd need posix_memalign /
        // aligned_alloc.  Reject over-aligned requests at runtime
        // rather than miscompile.
        if (@intFromEnum(alignment) > @sizeOf(usize) * 2) return null;
        const p = malloc(len) orelse return null;
        return @ptrCast(@alignCast(p));
    }

    fn resizeFn(
        _: *anyopaque,
        _: []u8,
        _: std.mem.Alignment,
        _: usize,
        _: usize,
    ) bool {
        // No in-place resize support — caller must alloc+memcpy+free.
        return false;
    }

    fn remapFn(
        _: *anyopaque,
        buf: []u8,
        _: std.mem.Alignment,
        new_len: usize,
        _: usize,
    ) ?[*]u8 {
        const p = realloc(buf.ptr, new_len) orelse return null;
        return @ptrCast(@alignCast(p));
    }

    fn freeFn(
        _: *anyopaque,
        buf: []u8,
        _: std.mem.Alignment,
        _: usize,
    ) void {
        free(buf.ptr);
    }
};

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
