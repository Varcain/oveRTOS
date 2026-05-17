// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Internal helpers for embedded-storage RTOS wrappers.
//!
//! oveRTOS Zig wrappers (Mutex, Semaphore, Thread, ...) embed kernel-object
//! storage as a struct field under `CONFIG_OVE_ZERO_HEAP=y`.  In heap mode
//! the storage field collapses to `void` (zero-sized, no overhead).
//!
//! ## Pinning contract
//!
//! After `init()`, the wrapper must remain at a stable address until
//! `deinit()`.  The kernel handle returned by `ove_*_init` references
//! `&self.storage` directly; moving or copying the wrapper invalidates that
//! pointer and produces silent corruption.
//!
//! `Tracker` (in debug builds — `std.debug.runtime_safety == true`) records
//! `&self` at `init()` time and lets methods assert the address has not
//! changed.  The check is compiled out in release builds with zero size or
//! runtime cost.
//!
//! ## Idiomatic usage at the call site
//!
//! ```zig
//! var mtx: ove.Mutex = undefined;
//! try mtx.init();           // if this fails, mtx stays `undefined` — do
//! defer mtx.deinit();        //   not register the defer above this line
//! try mtx.lock(timeout_ns);
//! ```

const std = @import("std");
const c = @import("c.zig").raw;

/// True when the build was configured with `CONFIG_OVE_ZERO_HEAP=y`.
pub const zero_heap = @hasDecl(c, "CONFIG_OVE_ZERO_HEAP");

/// Pin-tracking is enabled only in `Debug` builds (not `ReleaseSafe`).
/// `ReleaseSafe` is used by benchmarks and any other "release-quality
/// measurement" target — keeping the panic-dispatch symbol out of the
/// hot-path audit.  Users still catch moves in normal `Debug` testing.
const safety = (@import("builtin").mode == .Debug);

/// Address-tracking field embedded in every wrapper.  In release builds
/// this type is zero-sized and all methods compile to nothing; `@sizeOf`
/// of any wrapper that embeds a `Tracker` is identical between debug and
/// release.
pub const Tracker = if (safety) struct {
    addr: ?*const anyopaque = null,

    pub inline fn record(self: *@This(), p: *const anyopaque) void {
        self.addr = p;
    }

    pub inline fn clear(self: *@This()) void {
        self.addr = null;
    }

    pub inline fn assertSame(
        self: *const @This(),
        p: *const anyopaque,
        comptime type_name: []const u8,
    ) void {
        if (self.addr) |orig| {
            if (orig != p) {
                std.debug.panic(
                    "{s}: wrapper moved after init() — inited at {*}, used at {*}. " ++
                        "Embedded-storage RTOS wrappers must not be copied, moved, " ++
                        "passed by value, or stored in containers that relocate elements.",
                    .{ type_name, orig, p },
                );
            }
        } else {
            std.debug.panic(
                "{s}: method called before init() (or after deinit()) at {*}",
                .{ type_name, p },
            );
        }
    }
} else struct {
    pub inline fn record(_: *@This(), _: *const anyopaque) void {}
    pub inline fn clear(_: *@This()) void {}
    pub inline fn assertSame(_: *const @This(), _: *const anyopaque, comptime _: []const u8) void {}
};
