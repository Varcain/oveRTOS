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
//! - **Heap mode**: value-returning `spawn(...)`; the wrapper is just a
//!   handle, no embedded stack.  `stack_size` is forwarded to the kernel
//!   as a runtime hint.
//! - **Zero-heap mode**: two-phase `spawnStatic(self, ...)`; the stack
//!   is embedded as a struct field sized at comptime by `stack_size`.
//!   The wrapper must not be moved after `spawnStatic()` — debug builds
//!   panic on any method call from a different address; release builds
//!   compile the check out.
//!
//! Both spawn forms accept any callable via comptime introspection and
//! a tuple of runtime args.  If the callable's first parameter is
//! [`StopToken`], the trampoline injects a token referencing the new
//! thread; otherwise the args go straight through.  Cooperative
//! cancellation is opt-in via the entry signature:
//!
//! ```zig
//! // Cooperative worker — receives a StopToken; deinit auto-stops + joins.
//! fn worker(stop: ove.StopToken, queue: *Queue) void {
//!     while (!stop.isStopped()) { /* work */ }
//! }
//!
//! var th = try ove.Thread(4096).spawn(.{ .name = "worker" }, worker, .{&queue});
//! defer th.deinit();  // requestStop + destroy
//! ```
//!
//! Legacy fire-and-return entries (no token) are supported by simply
//! omitting the [`StopToken`] parameter.  Pass `.{}` for all config
//! defaults (anonymous name, normal priority):
//!
//! ```zig
//! fn oneshot() void { /* runs once and returns */ }
//! var th = try ove.Thread(4096).spawn(.{}, oneshot, .{});
//! defer th.deinit();
//! ```
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

/// Spawn-time configuration for a new thread.  Pass as `.{ ... }` to
/// [`Thread.spawn`] / [`Thread.spawnStatic`]; all fields are optional.
///
/// ```zig
/// // Common case — name + priority
/// try ove.Thread(4096).spawn(.{ .name = "worker", .priority = prio.high },
///                             entry, .{});
/// // Defaults — anonymous, normal priority
/// try ove.Thread(4096).spawn(.{}, entry, .{});
/// ```
///
/// The field is `comptime` at the call boundary because the name flows
/// through to the substrate as `[*:0]const u8` via comptime
/// nul-termination, and the priority is a constant tag.  Stack size is
/// at the type level (`Thread(stack_size)`), not here.
pub const SpawnConfig = struct {
    /// Thread name (defaults to `"ove-thread"`).  Nul-terminated at
    /// comptime before being handed to the substrate.
    name: ?[]const u8 = null,
    /// Scheduler priority (defaults to `prio.normal`).
    priority: Priority = prio.normal,
};

/// Comptime-produce a `[*:0]const u8` from a `SpawnConfig.name`.
/// Uses `std.fmt.comptimePrint`, which returns a sentinel-terminated
/// `*const [N:0]u8` backed by a comptime-static buffer — safe to pass
/// across the FFI boundary as a long-lived C string.
inline fn cfgNameZ(comptime cfg: SpawnConfig) [*:0]const u8 {
    const s = cfg.name orelse "ove-thread";
    return std.fmt.comptimePrint("{s}", .{s});
}

// ---------------------------------------------------------------------------
// StopToken — read-only handle to the per-thread cancellation flag.
// ---------------------------------------------------------------------------

/// Read-only handle to a thread's cooperative-cancellation flag.
///
/// Cheap to copy and pass by value.  Reads the per-thread atomic flag
/// set by [`Thread.requestStop`] (or implicitly by the wrapper's
/// `deinit`).  Workers spawned via `Thread(N).spawn(..., entry, ...)`
/// receive a token as the entry's first argument when the entry's
/// signature declares one.
///
/// ```zig
/// fn worker(stop: ove.StopToken) void {
///     while (!stop.isStopped()) {
///         // do work
///     }
/// }
///
/// var th = try ove.Thread(4096).spawn(.{ .name = "worker" }, worker, .{});
/// defer th.deinit();  // sets stop flag, then waits for worker exit
/// ```
pub const StopToken = struct {
    handle: c.ove_thread_t = null,

    /// Construct an empty token that never signals stop.  Useful as a
    /// default for fields filled in later.
    pub fn empty() StopToken {
        return .{ .handle = null };
    }

    /// `true` if [`Thread.requestStop`] has been called on the
    /// referenced thread.  Returns `false` for an empty token.
    pub inline fn isStopped(self: StopToken) bool {
        if (self.handle == null) return false;
        return c.ove_thread_should_stop(self.handle);
    }

    /// `true` if this token references a real thread (vs. [`empty`]).
    pub inline fn stopPossible(self: StopToken) bool {
        return self.handle != null;
    }

    /// Raw C handle accessor for advanced use.
    pub inline fn rawHandle(self: StopToken) c.ove_thread_t {
        return self.handle;
    }
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
// Trampoline generation (comptime introspection over entry signature)
// ---------------------------------------------------------------------------

/// Describes a `spawn` entry signature at comptime.  `takes_token` is
/// `true` when the first parameter is a [`StopToken`] (auto-injected
/// by the trampoline); `user_param_count` is 0 or 1.
const EntryInfo = struct {
    takes_token: bool,
    user_param_count: usize,
};

/// `true` for any single-machine-word pointer type — `*T`, `?*T`,
/// `[*]T`, `[*c]T`, and their optional/const variants.  Slices are
/// fat pointers (two words) and are rejected — they cannot be packed
/// into the substrate's `void *arg` slot.
fn isPointerArg(comptime T: type) bool {
    return switch (@typeInfo(T)) {
        .pointer => |p| p.size != .slice,
        .optional => |o| switch (@typeInfo(o.child)) {
            .pointer => |p| p.size != .slice,
            else => false,
        },
        else => false,
    };
}

inline fn introspect(comptime EntryFn: type, comptime ArgsT: type) EntryInfo {
    const fn_info = @typeInfo(EntryFn);
    if (fn_info != .@"fn") {
        @compileError("ove.Thread.spawn: `entry` must be a function, got " ++ @typeName(EntryFn));
    }
    const Ret = fn_info.@"fn".return_type orelse void;
    if (Ret != void) {
        @compileError(std.fmt.comptimePrint(
            "ove.Thread.spawn: `entry` must return `void`, got `{s}`. " ++
                "RTOS thread entries cannot propagate errors — the kernel ABI " ++
                "is `void(*)(void*)`.  Handle errors inside the entry body " ++
                "(try/catch, log, set a flag, etc.).",
            .{@typeName(Ret)},
        ));
    }
    const params = fn_info.@"fn".params;
    const takes_token = params.len > 0 and params[0].type != null and
        params[0].type.? == StopToken;
    const user_count = if (takes_token) params.len - 1 else params.len;

    const args_info = @typeInfo(ArgsT);
    if (args_info != .@"struct" or !args_info.@"struct".is_tuple) {
        @compileError("ove.Thread.spawn: `args` must be a tuple, got " ++ @typeName(ArgsT));
    }
    if (args_info.@"struct".fields.len != user_count) {
        @compileError(std.fmt.comptimePrint(
            "ove.Thread.spawn: entry takes {d} non-token parameter(s) but args tuple has {d} field(s)",
            .{ user_count, args_info.@"struct".fields.len },
        ));
    }
    if (user_count > 1) {
        @compileError("ove.Thread.spawn: entry can take at most one non-StopToken parameter " ++
            "(use a pointer to a struct if you need multiple values)");
    }

    if (user_count == 1) {
        const param_idx = if (takes_token) @as(usize, 1) else 0;
        const UserT = params[param_idx].type.?;
        if (!isPointerArg(UserT)) {
            @compileError("ove.Thread.spawn: entry's non-token parameter must be a " ++
                "pointer (`*T`, `?*T`, `[*]T`, `*anyopaque`, etc.).  Got `" ++
                @typeName(UserT) ++ "`.  The substrate ABI is `void(*)(void*)` — " ++
                "slices and non-pointer values cannot be packed into the single " ++
                "arg slot.  Pass a pointer to a struct holding your data instead.");
        }
        const ArgT = args_info.@"struct".fields[0].type;
        if (!isPointerArg(ArgT)) {
            @compileError("ove.Thread.spawn: args tuple element must be a pointer " ++
                "matching the entry's parameter.  Got `" ++ @typeName(ArgT) ++
                "`.  Pass `&value` or an existing pointer, not the value itself.");
        }
    }

    return .{
        .takes_token = takes_token,
        .user_param_count = user_count,
    };
}

/// Produce a `callconv(.c) fn(?*anyopaque) void` trampoline that
/// unpacks `args` and invokes `entry`, injecting a [`StopToken`] as the
/// first arg when the entry's signature requires it.
fn makeTrampoline(
    comptime entry: anytype,
    comptime info: anytype,
) type {
    return struct {
        fn invoke(arg: ?*anyopaque) callconv(.c) void {
            const fn_info = @typeInfo(@TypeOf(entry)).@"fn";
            if (info.takes_token) {
                // The substrate's per-thread "self" handle is published
                // by the parent AFTER the substrate spawn call returns,
                // on backends that need it (notably FreeRTOS via task
                // tag).  An equal-priority worker can outrace the parent
                // and see a null handle on first call — poll-yield until
                // it shows up.
                var h: c.ove_thread_t = c.ove_thread_get_self();
                while (h == null) {
                    c.ove_thread_yield();
                    h = c.ove_thread_get_self();
                }
                const tok = StopToken{ .handle = h };
                if (info.user_param_count == 0) {
                    entry(tok);
                } else {
                    const UserT = fn_info.params[1].type.?;
                    const ptr: UserT = @ptrCast(@alignCast(arg));
                    entry(tok, ptr);
                }
            } else {
                if (info.user_param_count == 0) {
                    entry();
                } else {
                    const UserT = fn_info.params[0].type.?;
                    const ptr: UserT = @ptrCast(@alignCast(arg));
                    entry(ptr);
                }
            }
        }
    };
}

inline fn ctxPointer(comptime user_param_count: usize, args: anytype) ?*anyopaque {
    if (user_param_count == 0) return null;
    return @ptrCast(args[0]);
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
/// var th = try ove.Thread(2048).spawn(.{ .name = "worker" }, workerEntry, .{});
/// defer th.deinit();
/// ```
///
/// In zero-heap mode the stack is embedded; two-phase spawn is required:
///
/// ```zig
/// var th: ove.Thread(2048) = undefined;
/// try th.spawnStatic(.{ .name = "worker" }, workerEntry, .{});
/// defer th.deinit();
/// ```
pub fn Thread(comptime stack_size: usize) type {
    return if (pin.zero_heap) ZeroHeapThread(stack_size) else HeapThread(stack_size);
}

fn HeapThread(comptime stack_size: usize) type {
    return struct {
        const Self = @This();

        handle: c.ove_thread_t,

        /// Spawn a thread.  `cfg` is a [`SpawnConfig`] literal (pass
        /// `.{}` for all defaults).  `entry` may take zero, one, or two
        /// parameters; the first may be a [`StopToken`] (auto-injected
        /// by the trampoline).  `args` is a tuple matching the
        /// remaining parameters (zero or one pointer).
        pub fn spawn(
            comptime cfg: SpawnConfig,
            comptime entry: anytype,
            args: anytype,
        ) Error!Self {
            const info = comptime introspect(@TypeOf(entry), @TypeOf(args));
            const Tramp = makeTrampoline(entry, info);
            const ctx_ptr = ctxPointer(info.user_param_count, args);
            var h: c.ove_thread_t = null;
            try err.fromCode(c.ove_thread_create(
                &h,
                comptime cfgNameZ(cfg),
                &Tramp.invoke,
                ctx_ptr,
                cfg.priority,
                stack_size,
            ));
            return .{ .handle = h };
        }

        /// Request cooperative cancellation.  Sets the per-thread atomic
        /// stop flag.  The worker must poll [`StopToken.isStopped`] for
        /// this to have any effect — the substrate does NOT
        /// force-terminate.  Safe from any context (ISR, other thread,
        /// the thread itself).  Idempotent.
        pub inline fn requestStop(self: Self) void {
            if (self.handle == null) return;
            c.ove_thread_request_stop(self.handle);
        }

        /// `true` if [`requestStop`] has been called on this thread.
        pub inline fn shouldStop(self: Self) bool {
            if (self.handle == null) return false;
            return c.ove_thread_should_stop(self.handle);
        }

        /// Get a [`StopToken`] referencing this thread's cancellation
        /// flag.  Cheap to copy; pass freely to helper functions.
        pub inline fn stopToken(self: Self) StopToken {
            return .{ .handle = self.handle };
        }

        /// Consume the wrapper without waiting for the worker to exit.
        /// The underlying kernel thread keeps running; its resources
        /// are leaked from the binding's perspective until the worker's
        /// entry function eventually returns.  Subsequent calls to
        /// `deinit` on a detached `Self` are no-ops.
        pub fn detach(self: *Self) void {
            self.handle = null;
        }

        /// Request cooperative stop, then wait for the worker to exit
        /// and release substrate resources.  Cooperative workers (those
        /// polling [`StopToken.isStopped`]) exit cleanly.  Workers that
        /// ignore the flag and don't return on their own block here
        /// indefinitely — the stop signal is set but nothing observes
        /// it.
        pub fn deinit(self: Self) void {
            if (self.handle == null) return;
            c.ove_thread_request_stop(self.handle);
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

        /// Spawn a thread into caller-provided embedded storage.  See
        /// [`HeapThread.spawn`] for the cfg/entry/args contract.  The
        /// wrapper must not be moved after this returns.
        pub fn spawnStatic(
            self: *Self,
            comptime cfg: SpawnConfig,
            comptime entry: anytype,
            args: anytype,
        ) Error!void {
            const info = comptime introspect(@TypeOf(entry), @TypeOf(args));
            const Tramp = makeTrampoline(entry, info);
            const ctx_ptr = ctxPointer(info.user_param_count, args);

            self.stack = [_]u8{0} ** stackTotal(stack_size);
            self.storage = std.mem.zeroes(c.ove_thread_storage_t);
            self.handle = null;
            self.tracker = .{};
            try err.fromCode(c.ove_thread_init(
                &self.handle,
                &self.storage,
                comptime cfgNameZ(cfg),
                &Tramp.invoke,
                ctx_ptr,
                cfg.priority,
                stack_size,
                &self.stack,
            ));
            self.tracker.record(self);
        }

        /// See [`HeapThread.requestStop`].
        pub inline fn requestStop(self: *Self) void {
            self.tracker.assertSame(self, "ove.Thread");
            if (self.handle == null) return;
            c.ove_thread_request_stop(self.handle);
        }

        /// See [`HeapThread.shouldStop`].
        pub inline fn shouldStop(self: *Self) bool {
            self.tracker.assertSame(self, "ove.Thread");
            if (self.handle == null) return false;
            return c.ove_thread_should_stop(self.handle);
        }

        /// See [`HeapThread.stopToken`].
        pub inline fn stopToken(self: *Self) StopToken {
            self.tracker.assertSame(self, "ove.Thread");
            return .{ .handle = self.handle };
        }

        /// **Not available in zero-heap mode.**  The wrapper owns the
        /// stack and storage that the kernel thread reads from, so
        /// dropping the wrapper while the thread still runs is UAF.
        /// `detach()` would silently invite that bug by removing the
        /// implicit wait that `deinit` provides.  Call [`deinit`]
        /// instead (it `request_stop`s, then waits), or hold the
        /// wrapper in file-scope/static storage that outlives the
        /// thread.
        pub fn detach(_: *Self) noreturn {
            @compileError("ove.Thread.detach is unsound in zero-heap mode: " ++
                "the wrapper owns the stack and storage that the kernel " ++
                "thread reads from.  Dropping the wrapper while the thread " ++
                "still runs is use-after-free.  Call deinit() instead, or " ++
                "hold the wrapper in file-scope/static storage that " ++
                "outlives the thread.");
        }

        pub fn deinit(self: *Self) void {
            self.tracker.assertSame(self, "ove.Thread");
            if (self.handle == null) return;
            c.ove_thread_request_stop(self.handle);
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
