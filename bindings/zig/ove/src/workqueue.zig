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
const pin = @import("pin.zig");
const Duration = @import("time.zig").Duration;

/// Deferred work queue backed by a dedicated RTOS thread.
///
/// `stack_size` is comptime so the worker stack can be embedded in zero-heap
/// builds; in heap mode it is forwarded to the kernel as a runtime hint.
///
/// Heap mode (value-returning create):
///
/// ```zig
/// var wq = try ove.Workqueue(2048).create("bench_wq", .normal);
/// defer wq.deinit();
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var wq: ove.Workqueue(2048) = undefined;
/// try wq.init("bench_wq", .normal);
/// defer wq.deinit();
/// ```
pub fn Workqueue(comptime stack_size: usize) type {
    return if (pin.zero_heap) ZeroHeapWorkqueue(stack_size) else HeapWorkqueue(stack_size);
}

fn HeapWorkqueue(comptime stack_size: usize) type {
    return struct {
        const Self = @This();

        handle: c.ove_workqueue_t,

        pub fn create(name: [*:0]const u8, priority: thread_mod.Priority) Error!Self {
            var h: c.ove_workqueue_t = null;
            try err.fromCode(c.ove_workqueue_create(&h, name, @intFromEnum(priority), stack_size));
            return .{ .handle = h };
        }

        pub fn deinit(self: Self) void {
            if (self.handle == null) return;
            c.ove_workqueue_destroy(self.handle);
        }

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

fn ZeroHeapWorkqueue(comptime stack_size: usize) type {
    // Zephyr MPU requires power-of-2 alignment + 128-byte guard pad on
    // every kernel thread stack — the workqueue's worker thread is no
    // exception.  Reuse `thread_mod.stackTotal/stackAlign` so we don't
    // re-derive the platform rules and the embedded stack matches what
    // `ove_workqueue_init` will pass to the underlying `k_work_queue_*`
    // API.  Without this, Zephyr ZH crashes inside `z_reset_time_slice`
    // on the first work-handler dispatch (PC inside `work_queue_main`),
    // observed on Rust+Zig benches.
    return struct {
        const Self = @This();

        stack: [thread_mod.stackTotal(stack_size)]u8 align(thread_mod.stackAlign(stack_size)),
        storage: c.ove_workqueue_storage_t,
        handle: c.ove_workqueue_t,
        tracker: pin.Tracker,

        pub fn init(self: *Self, name: [*:0]const u8, priority: thread_mod.Priority) Error!void {
            self.stack = [_]u8{0} ** thread_mod.stackTotal(stack_size);
            self.storage = std.mem.zeroes(c.ove_workqueue_storage_t);
            self.handle = null;
            self.tracker = .{};
            try err.fromCode(c.ove_workqueue_init(
                &self.handle,
                &self.storage,
                name,
                @intFromEnum(priority),
                stack_size,
                &self.stack,
            ));
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Workqueue");
            if (self.handle == null) return;
            c.ove_workqueue_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub fn submit(self: *Self, work: *Work) Error!void {
            self.tracker.assertSame(self, "ove.Workqueue");
            try err.fromCode(c.ove_work_submit(self.handle, work.handle));
        }

        pub fn submitDelayedFor(self: *Self, work: *Work, d: Duration) Error!void {
            self.tracker.assertSame(self, "ove.Workqueue");
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
/// Heap mode (value-returning create):
///
/// ```zig
/// var work = try ove.Work.create(myHandler);
/// defer work.deinit();
/// try wq.submit(&work);
/// ```
///
/// Zero-heap mode (two-phase init):
///
/// ```zig
/// var work: ove.Work = undefined;
/// try work.init(myHandler);
/// defer work.deinit();
/// ```
pub const Work = if (pin.zero_heap) ZeroHeapWork else HeapWork;

const HeapWork = struct {
    handle: c.ove_work_t,

    pub fn create(comptime handler: fn () void) Error!Work {
        const Tramp = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                handler();
            }
        };
        var h: c.ove_work_t = null;
        try err.fromCode(c.ove_work_init(&h, &Tramp.invoke));
        return .{ .handle = h };
    }

    /// Create with a typed context pointer.  `ctx` must outlive the work.
    pub fn createWithContext(
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
        var h: c.ove_work_t = null;
        try err.fromCode(c.ove_work_init(&h, &Captured.invoke));
        return .{ .handle = h };
    }

    pub fn deinit(self: Work) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_work_free"))
            c.ove_work_free(self.handle);
    }

    pub fn cancel(self: Work) Error!void {
        try err.fromCode(c.ove_work_cancel(self.handle));
    }
};

const ZeroHeapWork = struct {
    storage: c.ove_work_storage_t,
    handle: c.ove_work_t,
    tracker: pin.Tracker,

    pub fn init(self: *Work, comptime handler: fn () void) Error!void {
        const Tramp = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                handler();
            }
        };
        self.storage = std.mem.zeroes(c.ove_work_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_work_init_static(&self.handle, &self.storage, &Tramp.invoke));
        self.tracker.record(self);
    }

    /// Initialise with a typed context pointer.  `ctx` must outlive the work.
    pub fn initWithContext(
        self: *Work,
        comptime Context: type,
        ctx: *Context,
        comptime handler: fn (*Context) void,
    ) Error!void {
        const Captured = struct {
            var ctx_ptr: ?*Context = null;
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                if (ctx_ptr) |p| handler(p);
            }
        };
        Captured.ctx_ptr = ctx;
        self.storage = std.mem.zeroes(c.ove_work_storage_t);
        self.handle = null;
        self.tracker = .{};
        try err.fromCode(c.ove_work_init_static(&self.handle, &self.storage, &Captured.invoke));
        self.tracker.record(self);
    }

    pub fn deinit(self: *Work) void {
        self.tracker.assertSame(self, "ove.Work");
        if (self.handle == null) return;
        // No deinit/free for static work items — storage is owned by the wrapper.
        self.handle = null;
        self.tracker.clear();
    }

    pub fn cancel(self: *Work) Error!void {
        self.tracker.assertSame(self, "ove.Work");
        try err.fromCode(c.ove_work_cancel(self.handle));
    }
};
