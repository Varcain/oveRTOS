// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

const std = @import("std");
const c = @import("c.zig").raw;
const err = @import("error.zig");
const Error = err.Error;

const is_zephyr = @hasDecl(c, "CONFIG_OVE_RTOS_ZEPHYR");

/// Compute thread stack alignment. Zephyr MPU requires power-of-2 alignment
/// matching the total stack size (stack + 128-byte FPU guard, rounded up).
/// Other backends need 8-byte alignment (ARM AAPCS).
fn stackAlign(comptime stack_size: usize) comptime_int {
    if (is_zephyr) {
        return std.math.ceilPowerOfTwo(usize, stack_size + 128) catch 8192;
    }
    return 8;
}

/// Compute total stack allocation size for Zephyr (power-of-2 with guard).
fn stackTotal(comptime stack_size: usize) usize {
    if (is_zephyr) {
        return std.math.ceilPowerOfTwo(usize, stack_size + 128) catch 8192;
    }
    return stack_size;
}

/// Thread priority levels. Uses the C enum type directly for seamless FFI.
pub const Priority = c.ove_prio_t;

/// Predefined thread priority constants.
///
/// Use these instead of raw integer values for portability across RTOS backends.
pub const prio = struct {
    /// Lowest priority, typically used for the idle task.
    pub const idle: Priority = c.OVE_PRIO_IDLE;
    /// Low priority, for background tasks.
    pub const low: Priority = c.OVE_PRIO_LOW;
    /// Below-normal priority.
    pub const below_normal: Priority = c.OVE_PRIO_BELOW_NORMAL;
    /// Default task priority.
    pub const normal: Priority = c.OVE_PRIO_NORMAL;
    /// Above-normal priority for responsive tasks.
    pub const above_normal: Priority = c.OVE_PRIO_ABOVE_NORMAL;
    /// High priority for time-sensitive tasks.
    pub const high: Priority = c.OVE_PRIO_HIGH;
    /// Real-time priority; preempts most other tasks.
    pub const realtime: Priority = c.OVE_PRIO_REALTIME;
    /// Highest priority; use only for hard real-time ISR-adjacent tasks.
    pub const critical: Priority = c.OVE_PRIO_CRITICAL;
};

/// Thread state. Uses the C enum type directly for seamless FFI.
pub const State = c.ove_thread_state_t;

/// Runtime performance counters for a thread, returned by `getRuntimeStats()`.
pub const Stats = struct {
    /// Total CPU time consumed by this thread in microseconds.
    runtime_us: u64,
    /// CPU usage in hundredths of a percent (e.g. 1250 = 12.50%).
    cpu_percent_x100: u32,
};

/// RTOS thread handle.
///
/// Wraps the opaque `ove_thread_t` handle. Create with `spawn()` or
/// `spawnWithContext()`. Supports both heap and zero-heap backends.
pub const Thread = struct {
    handle: c.ove_thread_t,

    /// Spawn a thread with a Zig callback. Uses a trampoline to bridge
    /// the Zig `fn()` to the C `void(*)(void*)` signature.
    pub fn spawn(
        name: [*:0]const u8,
        comptime entry: fn () void,
        priority: Priority,
        comptime stack_size: usize,
    ) Error!Thread {
        // Trampoline captures `entry`, making it unique per entry function.
        // In zero-heap mode, storage and stack live inside Trampoline so
        // each entry function gets its own static allocation.
        const Trampoline = struct {
            fn invoke(_: ?*anyopaque) callconv(.c) void {
                entry();
            }
            var storage: c.ove_thread_storage_t = std.mem.zeroes(c.ove_thread_storage_t);
            var stack: [stackTotal(stack_size)]u8 align(stackAlign(stack_size)) = [_]u8{0} ** stackTotal(stack_size);
        };

        var h: c.ove_thread_t = null;
        if (comptime @hasDecl(c, "ove_thread_create_")) {
            const desc: c.struct_ove_thread_desc = .{
                .name = name,
                .entry = &Trampoline.invoke,
                .arg = null,
                .priority = priority,
                .stack_size = stack_size,
                .stack = null,
            };
            try err.fromCode(c.ove_thread_create_(&h, &desc));
        } else {
            const desc: c.struct_ove_thread_desc = .{
                .name = name,
                .entry = &Trampoline.invoke,
                .arg = null,
                .priority = priority,
                .stack_size = stack_size,
                .stack = &Trampoline.stack,
            };
            try err.fromCode(c.ove_thread_init(&h, &Trampoline.storage, &desc));
        }
        return .{ .handle = h };
    }

    /// Spawn a thread with a context pointer for closures / shared state.
    pub fn spawnWithContext(
        name: [*:0]const u8,
        comptime Context: type,
        ctx: *Context,
        comptime entry: fn (*Context) void,
        priority: Priority,
        comptime stack_size: usize,
    ) Error!Thread {
        const Trampoline = struct {
            fn invoke(arg: ?*anyopaque) callconv(.c) void {
                const ptr: *Context = @ptrCast(@alignCast(arg));
                entry(ptr);
            }
            var storage: c.ove_thread_storage_t = std.mem.zeroes(c.ove_thread_storage_t);
            var stack: [stackTotal(stack_size)]u8 align(stackAlign(stack_size)) = [_]u8{0} ** stackTotal(stack_size);
        };

        var h: c.ove_thread_t = null;
        if (comptime @hasDecl(c, "ove_thread_create_")) {
            const desc: c.struct_ove_thread_desc = .{
                .name = name,
                .entry = &Trampoline.invoke,
                .arg = @ptrCast(ctx),
                .priority = priority,
                .stack_size = stack_size,
                .stack = null,
            };
            try err.fromCode(c.ove_thread_create_(&h, &desc));
        } else {
            const desc: c.struct_ove_thread_desc = .{
                .name = name,
                .entry = &Trampoline.invoke,
                .arg = @ptrCast(ctx),
                .priority = priority,
                .stack_size = stack_size,
                .stack = &Trampoline.stack,
            };
            try err.fromCode(c.ove_thread_init(&h, &Trampoline.storage, &desc));
        }
        return .{ .handle = h };
    }

    /// Terminate and destroy the thread, releasing all RTOS resources.
    ///
    /// Sets `handle` to null. Safe to call on an already-destroyed thread.
    pub fn destroy(self: *Thread) void {
        if (self.handle == null) return;
        if (comptime @hasDecl(c, "ove_thread_destroy"))
            _ = c.ove_thread_destroy(self.handle)
        else
            _ = c.ove_thread_deinit(self.handle);
        self.handle = null;
    }

    /// Change the scheduling priority of this thread at runtime.
    pub fn setPriority(self: Thread, priority: Priority) void {
        c.ove_thread_set_priority(self.handle, priority);
    }

    /// Suspend this thread, preventing it from being scheduled.
    ///
    /// The thread remains suspended until `resumeThread()` is called.
    pub fn suspendThread(self: Thread) void {
        c.ove_thread_suspend(self.handle);
    }

    /// Resume a previously suspended thread, making it eligible to run again.
    pub fn resumeThread(self: Thread) void {
        c.ove_thread_resume(self.handle);
    }

    /// Return the high-water mark of stack usage for this thread in bytes.
    ///
    /// Useful for diagnosing stack overflow risk.
    pub fn getStackUsage(self: Thread) usize {
        return c.ove_thread_get_stack_usage(self.handle);
    }

    /// Return the current scheduling state of this thread.
    pub fn getState(self: Thread) State {
        return c.ove_thread_get_state(self.handle);
    }

    /// Return runtime CPU usage statistics for this thread.
    ///
    /// Returns `Error` if the RTOS does not support runtime stats collection.
    pub fn getRuntimeStats(self: Thread) Error!Stats {
        var raw_stats: c.struct_ove_thread_stats = undefined;
        try err.fromCode(c.ove_thread_get_runtime_stats(self.handle, &raw_stats));
        return .{
            .runtime_us = raw_stats.runtime_us,
            .cpu_percent_x100 = raw_stats.cpu_percent_x100,
        };
    }

    /// Return a handle to the currently executing thread.
    pub fn getSelf() Thread {
        return .{ .handle = c.ove_thread_get_self() };
    }

    /// Sleep the calling thread for `ms` milliseconds.
    ///
    /// Yields the CPU to other threads during the sleep period.
    pub fn sleepMs(ms: u32) void {
        c.ove_thread_sleep_ms(ms);
    }

    /// Yield the calling thread's remaining time slice to the scheduler.
    ///
    /// A hint to the RTOS to allow other threads of equal priority to run.
    pub fn yieldCpu() void {
        c.ove_thread_yield();
    }
};

// ---------------------------------------------------------------------------
// System memory statistics
// ---------------------------------------------------------------------------

/// System heap statistics.
pub const MemStats = struct {
    total: usize,
    free: usize,
    used: usize,
    peak_used: usize,
};

/// Query system heap statistics.
///
/// Returns `Error` if the RTOS does not support heap statistics.
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

/// Snapshot of a single thread's info.
pub const ThreadInfo = struct {
    name: [*:0]const u8,
    state: State,
    priority: i32,
    stack_used: usize,
};

/// List all threads in the system.
///
/// Returns a slice into `buf` with the actual number of threads found.
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
