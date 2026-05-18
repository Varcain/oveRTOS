// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const thread_mod = @import("thread.zig");
const Duration = @import("time.zig").Duration;

/// Deferred work queue backed by a dedicated RTOS thread.
///
/// `stack_size` is a comptime type parameter so the worker stack can be
/// laid out inline in the allocator-managed backing struct.  Both heap
/// and zero-heap modes use the substrate's `_init` path against a stack
/// + storage block drawn from the user's `std.mem.Allocator`.
///
/// ```zig
/// var wq = try ove.Workqueue(2048).create(allocator, "bench_wq", .normal);
/// defer wq.deinit();
/// ```
pub fn Workqueue(comptime stack_size: usize) type {
    return struct {
        const Self = @This();

        // Zephyr MPU requires power-of-2 alignment + 128-byte guard pad
        // on every kernel thread stack — reuse the thread module's
        // helpers so the embedded stack matches what `ove_workqueue_init`
        // expects.
        const Backing = struct {
            stack: [thread_mod.stackTotal(stack_size)]u8 align(thread_mod.stackAlign(stack_size)),
            storage: c.ove_workqueue_storage_t,
        };

        allocator: std.mem.Allocator,
        handle: c.ove_workqueue_t,
        backing: *Backing,

        /// Allocate the worker stack + substrate-storage from
        /// `allocator` and spawn a dedicated thread running
        /// `ove_workqueue_init` at `priority`.
        pub fn create(
            allocator: std.mem.Allocator,
            name: [*:0]const u8,
            priority: thread_mod.Priority,
        ) Error!Self {
            const backing = try allocator.create(Backing);
            errdefer allocator.destroy(backing);
            backing.stack = std.mem.zeroes(@TypeOf(backing.stack));
            backing.storage = std.mem.zeroes(c.ove_workqueue_storage_t);
            var h: c.ove_workqueue_t = null;
            try err.fromCode(c.ove_workqueue_init(
                &h,
                &backing.storage,
                name,
                @intFromEnum(priority),
                stack_size,
                &backing.stack,
            ));
            return .{ .allocator = allocator, .handle = h, .backing = backing };
        }

        pub fn deinit(self: Self) void {
            if (self.handle != null) c.ove_workqueue_deinit(self.handle);
            self.allocator.destroy(self.backing);
        }

        /// Enqueue `work` for execution by the workqueue thread.
        pub fn submit(self: Self, work: *Work) Error!void {
            try err.fromCode(c.ove_work_submit(self.handle, work.handle));
        }

        /// Submit `work` with a delay.  Substrate takes the delay as
        /// milliseconds; saturating-cast from the `Duration` ns count.
        pub fn submitDelayedFor(self: Self, work: *Work, d: Duration) Error!void {
            const ms: u32 = if (d.ns / std.time.ns_per_ms > std.math.maxInt(u32))
                std.math.maxInt(u32)
            else
                @intCast(d.ns / std.time.ns_per_ms);
            try err.fromCode(c.ove_work_submit_delayed(self.handle, work.handle, ms));
        }
    };
}

/// A single deferred work item that wraps a Zig callback.
///
/// ```zig
/// var work = try ove.Work.create(allocator, myHandler);
/// defer work.deinit();
/// try wq.submit(&work);
/// ```
pub const Work = struct {
    allocator: std.mem.Allocator,
    handle: c.ove_work_t,
    storage: *c.ove_work_storage_t,

    /// Create a deferred work item bound to a plain Zig callback.
    pub fn create(allocator: std.mem.Allocator, comptime handler: fn () void) Error!Work {
        const Tramp = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                handler();
            }
        };
        const storage = try allocator.create(c.ove_work_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_work_storage_t);
        var h: c.ove_work_t = null;
        try err.fromCode(c.ove_work_init_static(&h, storage, &Tramp.invoke));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    /// Create with a typed context pointer.  `ctx` must outlive the work.
    pub fn createWithContext(
        allocator: std.mem.Allocator,
        comptime Context: type,
        ctx: *Context,
        comptime handler: fn (*Context) void,
    ) Error!Work {
        const Captured = struct {
            var ctx_ptr: ?*Context = null;
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                if (ctx_ptr) |p| handler(p);
            }
        };
        Captured.ctx_ptr = ctx;
        const storage = try allocator.create(c.ove_work_storage_t);
        errdefer allocator.destroy(storage);
        storage.* = std.mem.zeroes(c.ove_work_storage_t);
        var h: c.ove_work_t = null;
        try err.fromCode(c.ove_work_init_static(&h, storage, &Captured.invoke));
        return .{ .allocator = allocator, .handle = h, .storage = storage };
    }

    pub fn deinit(self: Work) void {
        // Static work items have no substrate-level deinit; we just
        // release our storage.
        self.allocator.destroy(self.storage);
    }

    /// Cancel a pending submission.  No-op if the work has already
    /// run or is currently executing.
    pub fn cancel(self: Work) Error!void {
        try err.fromCode(c.ove_work_cancel(self.handle));
    }
};
