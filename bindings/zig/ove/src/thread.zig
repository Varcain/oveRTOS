// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Thread management.
//!
//! `Thread(stack_size)` is the templated wrapper.  Its shape differs by
//! build mode:
//!
//! - **Heap mode**: value-returning `create()`; the wrapper is just a
//!   handle, no embedded stack.  `stack_size` is forwarded to the kernel
//!   as a runtime hint.
//! - **Zero-heap mode**: two-phase `init()`; the stack is embedded as a
//!   struct field sized at comptime by `stack_size`.  The wrapper must
//!   not be moved after `init()` — debug builds panic on any method call
//!   from a different address; release builds compile the check out.
//!
//! Module-level helpers (`sleepMs`, `yieldCpu`, `getSelf`, `getMemStats`,
//! `threadList`, `Priority`, `prio`, `State`) are static — they don't bind
//! to an instance.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const pin = @import("pin.zig");

const is_zephyr = @hasDecl(c, "CONFIG_OVE_RTOS_ZEPHYR");

/// Compute thread stack alignment.  Zephyr MPU requires power-of-2
/// alignment matching the total stack size; other backends need 8-byte
/// alignment (ARM AAPCS).  Public so workqueue.zig can reuse the
/// platform-aware sizing for its embedded worker stack.
pub fn stackAlign(comptime stack_size: usize) comptime_int {
    if (is_zephyr) {
        return std.math.ceilPowerOfTwo(usize, stack_size + 128) catch 8192;
    }
    return 8;
}

pub fn stackTotal(comptime stack_size: usize) usize {
    if (is_zephyr) {
        return std.math.ceilPowerOfTwo(usize, stack_size + 128) catch 8192;
    }
    return stack_size;
}

/// Thread priority levels.  Use the `prio.*` constants.
pub const Priority = c.ove_prio_t;
pub const State = c.ove_thread_state_t;

pub const prio = struct {
    pub const idle: Priority = c.OVE_PRIO_IDLE;
    pub const low: Priority = c.OVE_PRIO_LOW;
    pub const below_normal: Priority = c.OVE_PRIO_BELOW_NORMAL;
    pub const normal: Priority = c.OVE_PRIO_NORMAL;
    pub const above_normal: Priority = c.OVE_PRIO_ABOVE_NORMAL;
    pub const high: Priority = c.OVE_PRIO_HIGH;
    pub const realtime: Priority = c.OVE_PRIO_REALTIME;
    pub const critical: Priority = c.OVE_PRIO_CRITICAL;
};

pub const Stats = struct {
    runtime_us: u64,
    cpu_percent_x100: u32,
};

// ---------------------------------------------------------------------------
// Module-level helpers (no instance)
// ---------------------------------------------------------------------------

/// Sleep the calling thread for `ms` milliseconds.
pub inline fn sleepMs(ms: u32) void {
    c.ove_thread_sleep_ms(ms);
}

/// Yield the remaining time slice to the scheduler.
pub inline fn yieldCpu() void {
    c.ove_thread_yield();
}

/// Return the raw C handle of the currently executing thread.  Use only
/// for FFI — the wrapper type cannot be reconstructed from a bare handle
/// because the storage is owned by a `Thread(N)` value somewhere.
pub fn getSelf() c.ove_thread_t {
    return c.ove_thread_get_self();
}

// ---------------------------------------------------------------------------
// Thread(stack_size)
// ---------------------------------------------------------------------------

/// RTOS thread.  `stack_size` is the user-requested stack in bytes; the
/// actual allocation may be larger to satisfy Zephyr MPU alignment.
///
/// In heap mode the wrapper is a single handle; the kernel allocates the
/// stack from the heap based on `stack_size`:
///
/// ```zig
/// var th = try ove.Thread(2048).create("worker", workerEntry, .normal);
/// defer th.deinit();
/// ```
///
/// In zero-heap mode the stack is embedded; two-phase init is required:
///
/// ```zig
/// var th: ove.Thread(2048) = undefined;
/// try th.init("worker", workerEntry, .normal);
/// defer th.deinit();
/// ```
pub fn Thread(comptime stack_size: usize) type {
    return if (pin.zero_heap) ZeroHeapThread(stack_size) else HeapThread(stack_size);
}

fn HeapThread(comptime stack_size: usize) type {
    return struct {
        const Self = @This();

        handle: c.ove_thread_t,

        /// Create with a Zig callback (no context).
        pub fn create(
            name: [*:0]const u8,
            comptime entry: fn () void,
            priority: Priority,
        ) Error!Self {
            const Tramp = struct {
                fn invoke(_: ?*anyopaque) callconv(.c) void {
                    entry();
                }
            };
            var h: c.ove_thread_t = null;
            try err.fromCode(c.ove_thread_create(&h, name, &Tramp.invoke, null, priority, stack_size));
            return .{ .handle = h };
        }

        /// Create with a typed context pointer.  `ctx` must outlive the thread.
        pub fn createWithContext(
            name: [*:0]const u8,
            comptime Context: type,
            ctx: *Context,
            comptime entry: fn (*Context) void,
            priority: Priority,
        ) Error!Self {
            const Tramp = struct {
                fn invoke(arg: ?*anyopaque) callconv(.c) void {
                    const ptr: *Context = @ptrCast(@alignCast(arg));
                    entry(ptr);
                }
            };
            var h: c.ove_thread_t = null;
            try err.fromCode(c.ove_thread_create(&h, name, &Tramp.invoke, @ptrCast(ctx), priority, stack_size));
            return .{ .handle = h };
        }

        pub fn deinit(self: Self) void {
            if (self.handle == null) return;
            _ = c.ove_thread_destroy(self.handle);
        }

        pub fn setPriority(self: Self, priority: Priority) void {
            c.ove_thread_set_priority(self.handle, priority);
        }

        pub fn suspendThread(self: Self) void {
            c.ove_thread_suspend(self.handle);
        }

        pub fn resumeThread(self: Self) void {
            c.ove_thread_resume(self.handle);
        }

        pub fn getStackUsage(self: Self) usize {
            return c.ove_thread_get_stack_usage(self.handle);
        }

        pub fn getState(self: Self) State {
            return c.ove_thread_get_state(self.handle);
        }

        pub fn getRuntimeStats(self: Self) Error!Stats {
            var raw_stats: c.struct_ove_thread_stats = undefined;
            try err.fromCode(c.ove_thread_get_runtime_stats(self.handle, &raw_stats));
            return .{
                .runtime_us = raw_stats.runtime_us,
                .cpu_percent_x100 = raw_stats.cpu_percent_x100,
            };
        }
    };
}

fn ZeroHeapThread(comptime stack_size: usize) type {
    return struct {
        const Self = @This();

        stack: [stackTotal(stack_size)]u8 align(stackAlign(stack_size)),
        storage: c.ove_thread_storage_t,
        handle: c.ove_thread_t,
        tracker: pin.Tracker,

        /// Initialise with a Zig callback (no context).
        pub fn init(
            self: *Self,
            name: [*:0]const u8,
            comptime entry: fn () void,
            priority: Priority,
        ) Error!void {
            const Tramp = struct {
                fn invoke(_: ?*anyopaque) callconv(.c) void {
                    entry();
                }
            };
            self.stack = [_]u8{0} ** stackTotal(stack_size);
            self.storage = std.mem.zeroes(c.ove_thread_storage_t);
            self.handle = null;
            self.tracker = .{};
            try err.fromCode(c.ove_thread_init(&self.handle, &self.storage, name, &Tramp.invoke, null, priority, stack_size, &self.stack));
            self.tracker.record(self);
        }

        /// Initialise with a typed context pointer.  `ctx` must outlive the thread.
        pub fn initWithContext(
            self: *Self,
            name: [*:0]const u8,
            comptime Context: type,
            ctx: *Context,
            comptime entry: fn (*Context) void,
            priority: Priority,
        ) Error!void {
            const Tramp = struct {
                fn invoke(arg: ?*anyopaque) callconv(.c) void {
                    const ptr: *Context = @ptrCast(@alignCast(arg));
                    entry(ptr);
                }
            };
            self.stack = [_]u8{0} ** stackTotal(stack_size);
            self.storage = std.mem.zeroes(c.ove_thread_storage_t);
            self.handle = null;
            self.tracker = .{};
            try err.fromCode(c.ove_thread_init(&self.handle, &self.storage, name, &Tramp.invoke, @ptrCast(ctx), priority, stack_size, &self.stack));
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Thread");
            if (self.handle == null) return;
            _ = c.ove_thread_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub fn setPriority(self: *Self, priority: Priority) void {
            self.tracker.assertSame(self, "ove.Thread");
            c.ove_thread_set_priority(self.handle, priority);
        }

        pub fn suspendThread(self: *Self) void {
            self.tracker.assertSame(self, "ove.Thread");
            c.ove_thread_suspend(self.handle);
        }

        pub fn resumeThread(self: *Self) void {
            self.tracker.assertSame(self, "ove.Thread");
            c.ove_thread_resume(self.handle);
        }

        pub fn getStackUsage(self: *Self) usize {
            self.tracker.assertSame(self, "ove.Thread");
            return c.ove_thread_get_stack_usage(self.handle);
        }

        pub fn getState(self: *Self) State {
            self.tracker.assertSame(self, "ove.Thread");
            return c.ove_thread_get_state(self.handle);
        }

        pub fn getRuntimeStats(self: *Self) Error!Stats {
            self.tracker.assertSame(self, "ove.Thread");
            var raw_stats: c.struct_ove_thread_stats = undefined;
            try err.fromCode(c.ove_thread_get_runtime_stats(self.handle, &raw_stats));
            return .{
                .runtime_us = raw_stats.runtime_us,
                .cpu_percent_x100 = raw_stats.cpu_percent_x100,
            };
        }
    };
}

// ---------------------------------------------------------------------------
// System memory statistics
// ---------------------------------------------------------------------------

pub const MemStats = struct {
    total: usize,
    free: usize,
    used: usize,
    peak_used: usize,
};

pub fn getMemStats() Error!MemStats {
    var raw: c.struct_ove_mem_stats = undefined;
    try err.fromCode(c.ove_sys_get_mem_stats(&raw));
    return .{
        .total = raw.total,
        .free = raw.free,
        .used = raw.used,
        .peak_used = raw.peak_used,
    };
}

// ---------------------------------------------------------------------------
// Thread enumeration
// ---------------------------------------------------------------------------

pub const ThreadInfo = struct {
    name: [*:0]const u8,
    state: State,
    priority: i32,
    stack_used: usize,
};

pub fn threadList(buf: []ThreadInfo) Error![]ThreadInfo {
    const max = @min(buf.len, 16);
    var raw: [16]c.struct_ove_thread_info = std.mem.zeroes([16]c.struct_ove_thread_info);
    var actual: usize = 0;
    try err.fromCode(c.ove_thread_list(&raw, max, &actual));
    for (0..actual) |i| {
        buf[i] = .{
            .name = @ptrCast(raw[i].name),
            .state = raw[i].state,
            .priority = raw[i].priority,
            .stack_used = raw[i].stack_used,
        };
    }
    return buf[0..actual];
}
