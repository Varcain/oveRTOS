// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Workqueue — a dedicated thread that runs deferred `Work` items submitted
//! from other threads.  Submitting is not ISR-safe on FreeRTOS or NuttX; from
//! an interrupt, wake a thread and submit from it.
//!
//! Wraps `ove/workqueue.h`. Create one queue per priority class with
//! `Workqueue.create(allocator, …)`, then `submit(wq, work)` to defer
//! execution off the producer's stack. `submitDelayedFor` schedules the work to
//! run after a `Duration`. Available when `CONFIG_OVE_WORKQUEUE` is enabled.

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
        backing: ?*Backing,

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
            // Don't zero `backing.stack` — same rationale as ove.Thread.spawn:
            // it's the workqueue worker thread's stack, substrate-initialised
            // and RTOS-fill-pattern for usage accounting, so pre-zeroing the
            // multi-KiB buffer is pure overhead (dominated workqueue
            // create_destroy).  Only the control block needs zeroing.
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

        /// Idempotent — clears `handle` and `backing` after teardown so a
        /// redundant `defer wq.deinit()` after an explicit `deinit()` is a
        /// safe no-op rather than a double free.
        pub fn deinit(self: *Self) void {
            if (self.handle) |h| {
                c.ove_workqueue_deinit(h);
                self.handle = null;
            }
            if (self.backing) |b| {
                self.allocator.destroy(b);
                self.backing = null;
            }
        }

        /// Enqueue `work` for execution by the workqueue thread. Not ISR-safe
        /// on FreeRTOS or NuttX; on a full queue FreeRTOS fails at once and
        /// NuttX waits for room.
        pub fn submit(self: Self, work: *Work) Error!void {
            try err.fromCode(c.ove_work_submit(self.handle, work.handle));
        }

        /// Submit `work` with a delay.  Substrate takes the delay as
        /// milliseconds; saturating-cast from the `Duration` ns count.  Not
        /// guaranteed today: FreeRTOS drops the item if the queue is full when
        /// the delay expires, and NuttX sleeps the delay on the worker after
        /// dequeue, holding up the items behind it.
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
    box: ?*Box,

    /// Substrate storage plus the per-item context.  `ove_work_init_static`
    /// hands the storage address back as the handle, so a trampoline recovers
    /// its own item with `@fieldParentPtr` instead of a shared static.
    const Box = struct {
        storage: c.ove_work_storage_t,
        ctx: ?*anyopaque = null,
    };

    fn boxOf(h: c.ove_work_t) *Box {
        const storage: *c.ove_work_storage_t = @ptrCast(@alignCast(h.?));
        return @fieldParentPtr("storage", storage);
    }

    fn init(allocator: std.mem.Allocator, ctx: ?*anyopaque, invoke: c.ove_work_fn) Error!Work {
        const box = try allocator.create(Box);
        errdefer allocator.destroy(box);
        box.* = .{ .storage = std.mem.zeroes(c.ove_work_storage_t), .ctx = ctx };
        var h: c.ove_work_t = null;
        try err.fromCode(c.ove_work_init_static(&h, &box.storage, invoke));
        return .{ .allocator = allocator, .handle = h, .box = box };
    }

    /// Create a deferred work item bound to a plain Zig callback.
    pub fn create(allocator: std.mem.Allocator, comptime handler: fn () void) Error!Work {
        const Tramp = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                handler();
            }
        };
        return init(allocator, null, &Tramp.invoke);
    }

    /// Create with a typed context pointer.  `ctx` must outlive the work.
    pub fn createWithContext(
        allocator: std.mem.Allocator,
        comptime Context: type,
        ctx: *Context,
        comptime handler: fn (*Context) void,
    ) Error!Work {
        const Tramp = struct {
            fn invoke(h: c.ove_work_t) callconv(.c) void {
                handler(@ptrCast(@alignCast(boxOf(h).ctx.?)));
            }
        };
        return init(allocator, ctx, &Tramp.invoke);
    }

    /// Idempotent — drains the item before releasing its caller-owned storage,
    /// then clears it so a redundant
    /// `defer work.deinit()` after an explicit `deinit()` is a safe no-op
    /// rather than a double free.
    pub fn deinit(self: *Work) void {
        if (self.box) |b| {
            c.ove_work_deinit(self.handle);
            self.handle = null;
            self.allocator.destroy(b);
            self.box = null;
        }
    }

    /// Cancel a pending submission.  No-op if the work has already
    /// run or is currently executing.
    pub fn cancel(self: Work) Error!void {
        try err.fromCode(c.ove_work_cancel(self.handle));
    }
};
