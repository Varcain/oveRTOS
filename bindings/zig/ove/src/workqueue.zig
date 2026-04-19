// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;
const thread = @import("thread.zig");

/// Deferred work queue backed by a dedicated RTOS thread.
///
/// Submit `Work` items to execute callbacks asynchronously on the queue's
/// thread. Supports both heap and zero-heap backends.
pub const Workqueue = struct {
    handle: c.ove_workqueue_t,

    /// Create a work queue with its own thread of the given priority and stack size.
    ///
    /// `name` is the null-terminated thread name shown in debugger/stats output.
    /// `stack_size` is a comptime constant (bytes) for zero-heap stack allocation.
    /// In zero-heap mode, the storage and stack are comptime-unique static variables.
    /// Returns `Error` if the RTOS fails to create the work queue thread.
    pub fn create(
        name: [*:0]const u8,
        priority: thread.Priority,
        comptime stack_size: usize,
    ) Error!Workqueue {
        var h: c.ove_workqueue_t = null;
        if (comptime @hasDecl(c, "ove_workqueue_create")) {
            try err.fromCode(c.ove_workqueue_create(
                &h,
                name,
                priority,
                stack_size,
            ));
        } else {
            const S = struct {
                var storage: c.ove_workqueue_storage_t = std.mem.zeroes(c.ove_workqueue_storage_t);
                var stack: [stack_size]u8 align(8) = [_]u8{0} ** stack_size;
            };
            try err.fromCode(c.ove_workqueue_init(
                &h,
                &S.storage,
                name,
                priority,
                stack_size,
                &S.stack,
            ));
        }
        return .{ .handle = h };
    }

    /// Destroy the work queue and terminate its thread.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed work queue.
    pub fn destroy(self: *Workqueue) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_workqueue_destroy"))
            c.ove_workqueue_destroy(self.handle)
        else
            c.ove_workqueue_deinit(self.handle);
        self.handle = null;
    }

    /// Submit a `Work` item to this queue for immediate execution.
    ///
    /// The `work` callback runs on the queue's thread as soon as it is scheduled.
    /// Returns `Error` if the submission fails.
    pub fn submit(self: Workqueue, work: Work) Error!void {
        try err.fromCode(c.ove_work_submit(self.handle, work.handle));
    }

    /// Submit a `Work` item to this queue with a deferred start after `delay_ms`.
    ///
    /// The callback runs on the queue's thread at least `delay_ms` milliseconds
    /// after this call. Returns `Error` if the submission fails.
    pub fn submitDelayed(self: Workqueue, work: Work, delay_ms: u32) Error!void {
        try err.fromCode(c.ove_work_submit_delayed(self.handle, work.handle, delay_ms));
    }
};

/// A single deferred work item that wraps a Zig callback.
///
/// Create with `Work.create()` and submit to a `Workqueue`. Each `Work`
/// instance can be re-submitted after its callback completes.
/// Supports both heap and zero-heap backends.
pub const Work = struct {
    handle: c.ove_work_t,

    /// Create a work item wrapping a zero-argument Zig callback.
    ///
    /// In zero-heap mode, the internal storage is a comptime-unique static variable.
    /// Returns `Error` if the RTOS fails to allocate the work item.
    pub fn create(comptime handler: fn () void) Error!Work {
        const Trampoline = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                handler();
            }
        };

        var h: c.ove_work_t = null;
        if (comptime @hasDecl(c, "ove_work_init")) {
            try err.fromCode(c.ove_work_init(&h, &Trampoline.invoke));
        } else {
            const S = struct {
                var storage: c.ove_work_storage_t = std.mem.zeroes(c.ove_work_storage_t);
            };
            try err.fromCode(c.ove_work_init_static(&h, &S.storage, &Trampoline.invoke));
        }
        return .{ .handle = h };
    }

    /// Create a work item with a typed context pointer.
    ///
    /// The oveRTOS C work handler has no `user_data` slot, so the context
    /// pointer is stashed in a per-call-site comptime-unique static. Each
    /// source location that calls `createWithContext` gets exactly one
    /// context slot; calling it twice from the same location will overwrite
    /// the previous context. Use distinct call sites (e.g. separate wrapper
    /// functions) when you need multiple instances.
    ///
    /// `ctx` must outlive the `Work` item.
    pub fn createWithContext(
        comptime Context: type,
        ctx: *Context,
        comptime handler: fn (*Context) void,
    ) Error!Work {
        const Captured = struct {
            var ctx_ptr: ?*Context = null;
        };
        Captured.ctx_ptr = ctx;
        const Trampoline = struct {
            fn invoke(_: c.ove_work_t) callconv(.c) void {
                if (Captured.ctx_ptr) |p| handler(p);
            }
        };

        var h: c.ove_work_t = null;
        if (comptime @hasDecl(c, "ove_work_init")) {
            try err.fromCode(c.ove_work_init(&h, &Trampoline.invoke));
        } else {
            const S = struct {
                var storage: c.ove_work_storage_t = std.mem.zeroes(c.ove_work_storage_t);
            };
            try err.fromCode(c.ove_work_init_static(&h, &S.storage, &Trampoline.invoke));
        }
        return .{ .handle = h };
    }

    /// Free resources associated with this work item.
    ///
    /// Sets `handle` to null. Only has an effect on heap-backed backends.
    /// Do not call while the work item may be pending on a queue.
    pub fn free(self: *Work) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_work_free"))
            c.ove_work_free(self.handle);
        self.handle = null;
    }

    /// Cancel a pending work item before it executes.
    ///
    /// Has no effect if the work has already started. Returns `Error` if the
    /// cancellation fails.
    pub fn cancel(self: Work) Error!void {
        try err.fromCode(c.ove_work_cancel(self.handle));
    }
};
