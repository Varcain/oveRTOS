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

/// Deferred work queue backed by a dedicated RTOS thread.
///
/// `stack_size` is comptime so the worker stack can be embedded in zero-heap
/// builds.  In heap mode the stack field is zero-sized.
///
/// ```zig
/// var wq: ove.Workqueue(2048) = undefined;
/// try wq.init("bench_wq", .normal);
/// defer wq.deinit();
/// ```
pub fn Workqueue(comptime stack_size: usize) type {
    // Zephyr MPU requires power-of-2 alignment + 128-byte guard pad on
    // every kernel thread stack — the workqueue's worker thread is no
    // exception.  Reuse `thread_mod.stackTotal/stackAlign` so we don't
    // re-derive the platform rules and the embedded stack matches what
    // `ove_workqueue_init` will pass to the underlying `k_work_queue_*`
    // API.  Without this, Zephyr ZH crashes inside `z_reset_time_slice`
    // on the first work-handler dispatch (PC inside `work_queue_main`),
    // observed on Rust+Zig benches.
    const Stack = if (pin.zero_heap)
        [thread_mod.stackTotal(stack_size)]u8
    else
        void;

    return struct {
        const Self = @This();

        stack: Stack align(if (pin.zero_heap) thread_mod.stackAlign(stack_size) else 1),
        storage: pin.Storage(c.ove_workqueue_storage_t),
        handle: c.ove_workqueue_t,
        tracker: pin.Tracker,

        pub fn init(self: *Self, name: [*:0]const u8, priority: thread_mod.Priority) Error!void {
            if (comptime pin.zero_heap) {
                self.stack = [_]u8{0} ** thread_mod.stackTotal(stack_size);
            } else {
                self.stack = {};
            }
            self.storage = pin.zeroStorage(c.ove_workqueue_storage_t);
            self.handle = null;
            self.tracker = .{};
            if (comptime !pin.zero_heap) {
                try err.fromCode(c.ove_workqueue_create(&self.handle, name, priority, stack_size));
            } else {
                try err.fromCode(c.ove_workqueue_init(
                    &self.handle,
                    &self.storage,
                    name,
                    priority,
                    stack_size,
                    &self.stack,
                ));
            }
            self.tracker.record(self);
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Workqueue");
            if (self.handle == null) return;
            if (comptime !pin.zero_heap)
                c.ove_workqueue_destroy(self.handle)
            else
                c.ove_workqueue_deinit(self.handle);
            self.handle = null;
            self.tracker.clear();
        }

        pub fn submit(self: *Self, work: *Work) Error!void {
            self.tracker.assertSame(self, "ove.Workqueue");
            try err.fromCode(c.ove_work_submit(self.handle, work.handle));
        }

        pub fn submitDelayed(self: *Self, work: *Work, delay_ms: u32) Error!void {
            self.tracker.assertSame(self, "ove.Workqueue");
            try err.fromCode(c.ove_work_submit_delayed(self.handle, work.handle, delay_ms));
        }
    };
}

/// A single deferred work item that wraps a Zig callback.
///
/// ```zig
/// var work: ove.Work = undefined;
/// try work.init(myHandler);
/// defer work.deinit();
/// try wq.submit(&work);
/// ```
pub const Work = struct {
    storage: pin.Storage(c.ove_work_storage_t),
    handle: c.ove_work_t,
    tracker: pin.Tracker,

    pub fn init(self: *Work, comptime handler: fn () void) Error!void {
        const Tramp = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                handler();
            }
        };
        self.storage = pin.zeroStorage(c.ove_work_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_work_init(&self.handle, &Tramp.invoke));
        } else {
            try err.fromCode(c.ove_work_init_static(&self.handle, &self.storage, &Tramp.invoke));
        }
        self.tracker.record(self);
    }

    /// Initialise with a typed context pointer.  `ctx` must outlive the work.
    pub fn initWithContext(
        self: *Work,
        comptime Context: type,
        ctx: *Context,
        comptime handler: fn (*Context) void,
    ) Error!void {
        // Per-Self captured context — stored in a static so the C handler
        // can reach it (oveRTOS work API has no user_data slot).  Each
        // Work instance stamps its own ctx into this static at init() time;
        // re-using the same `Work` with a different ctx works as long as
        // submit/handler/init don't race.
        const Captured = struct {
            var ctx_ptr: ?*Context = null;
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                if (ctx_ptr) |p| handler(p);
            }
        };
        Captured.ctx_ptr = ctx;
        self.storage = pin.zeroStorage(c.ove_work_storage_t);
        self.handle = null;
        self.tracker = .{};
        if (comptime !pin.zero_heap) {
            try err.fromCode(c.ove_work_init(&self.handle, &Captured.invoke));
        } else {
            try err.fromCode(c.ove_work_init_static(&self.handle, &self.storage, &Captured.invoke));
        }
        self.tracker.record(self);
    }

    pub fn deinit(self: *Work) void {
        self.tracker.assertSame(self, "ove.Work");
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_work_free"))
            c.ove_work_free(self.handle);
        self.handle = null;
        self.tracker.clear();
    }

    pub fn cancel(self: *Work) Error!void {
        self.tracker.assertSame(self, "ove.Work");
        try err.fromCode(c.ove_work_cancel(self.handle));
    }
};
