// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Thread management.
//!
//! `Thread(stack_size)` is the templated wrapper.  The substrate-storage
//! block (stack + `ove_thread_storage_t`) lives in caller-supplied
//! allocator memory; the wrapper itself holds an `Allocator` plus a
//! kernel handle and is movable by value.  Heap and zero-heap modes
//! share the same shape — they differ only in which allocator the
//! caller passes in (libc-backed vs. `FixedBufferAllocator` over BSS).
//!
//! `stack_size` is a comptime parameter so the backing struct can size
//! its embedded stack array inline; on Zephyr the binding rounds up to
//! a power-of-two with a 128-byte MPU guard (see [`stackTotal`]).
//!
//! `spawn` accepts any callable via comptime introspection and a tuple
//! of runtime args.  If the callable's first parameter is [`StopToken`],
//! the trampoline injects a token referencing the new thread; otherwise
//! the args go straight through.  Cooperative cancellation is opt-in
//! via the entry signature:
//!
//! ```zig
//! // Cooperative worker — receives a StopToken; deinit auto-stops + joins.
//! fn worker(stop: ove.StopToken, queue: *Queue) void {
//!     while (!stop.isStopped()) { /* work */ }
//! }
//!
//! var th = try ove.Thread(4096).spawn(allocator, .{ .name = "worker" }, worker, .{&queue});
//! defer th.deinit();  // requestStop + destroy
//! ```
//!
//! Legacy fire-and-return entries (no token) are supported by simply
//! omitting the [`StopToken`] parameter.  Pass `.{}` for all config
//! defaults (anonymous name, normal priority):
//!
//! ```zig
//! fn oneshot() void { /* runs once and returns */ }
//! var th = try ove.Thread(4096).spawn(allocator, .{}, oneshot, .{});
//! defer th.deinit();
//! ```
//!
//! ## Passing multiple values to the entry
//!
//! The substrate's thread ABI is `void(*)(void*)` — a single
//! machine-word `void *arg` slot.  The binding mirrors that exactly:
//! the entry can take zero or one non-token pointer parameter, and the
//! `args` tuple has zero or one pointer element.  To hand a worker
//! multiple values, define a context struct and pass `&ctx`:
//!
//! ```zig
//! const Ctx = struct {
//!     queue: *ove.Queue(Msg, 32),
//!     mutex: *ove.Mutex,
//!     channel_id: u32,
//! };
//!
//! fn worker(stop: ove.StopToken, ctx: *Ctx) void {
//!     while (!stop.isStopped()) {
//!         // ctx.queue, ctx.mutex, ctx.channel_id all available
//!     }
//! }
//!
//! var ctx = Ctx{ .queue = &q, .mutex = &mtx, .channel_id = 7 };
//! var th = try ove.Thread(4096).spawn(allocator, .{ .name = "worker" }, worker, .{&ctx});
//! defer th.deinit();   // worker exits before ctx goes out of scope
//! ```
//!
//! `ctx` must outlive the thread.  The simplest discipline is to declare
//! it at the same (or wider) scope as the wrapper, so `defer th.deinit()`
//! waits for the worker to exit before `ctx` is popped.  For long-lived
//! threads, place `ctx` in file-scope storage.
//!
//! This binding deliberately does *not* heap-box arbitrary args tuples
//! the way `std.Thread.spawn` does — the substrate's single-pointer ABI
//! is a real constraint, hidden allocations would conflict with the
//! zero-heap mode and the binding's broader "make allocation explicit"
//! contract, and the workaround above is one line.
//!
//! Module-level helpers (`sleepMs`, `yieldCpu`, `getSelf`, `getMemStats`,
//! `threadList`, `Priority`, `State`) are static — they don't bind
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

/// Thread priority levels.
///
/// `enum(c_uint)` matching the substrate's `ove_prio_t` C enum exactly
/// (the C side translates as `c_uint` since all variants are
/// non-negative), so the FFI conversion at the boundary is
/// `@intFromEnum(p)` — a no-op at the machine-code level.  Pin block
/// below catches substrate drift at compile time.
///
/// ```zig
/// try ove.Thread(4096).spawn(allocator, .{ .name = "worker", .priority = .high },
///                             entry, .{});
/// ```
pub const Priority = enum(c_uint) {
    /// Lowest priority; runs only when no other thread is ready.
    idle = 0,
    /// Low priority background work.
    low = 1,
    /// Below-normal priority.
    below_normal = 2,
    /// Default application priority.
    normal = 3,
    /// Above-normal priority.
    above_normal = 4,
    /// High priority; prefer for time-sensitive tasks.
    high = 5,
    /// Real-time priority; use with care.
    realtime = 6,
    /// Highest priority; reserved for critical system tasks.
    critical = 7,
};
/// Scheduler-observable thread state.  Re-exported as the raw C enum
/// (`c.ove_thread_state_t`); compare against the substrate's variants:
/// `c.OVE_THREAD_STATE_RUNNING` (on CPU), `_READY` (runnable, waiting),
/// `_BLOCKED` (waiting on sync primitive / delay), `_SUSPENDED` (explicit
/// `Thread.suspend`), `_TERMINATED` (entry returned, not yet destroyed),
/// or `_UNKNOWN`.
pub const State = c.ove_thread_state_t;

comptime {
    // Pin enum values against the substrate's `OVE_PRIO_*` defines.
    // If the substrate renumbers a variant, this fails to compile.
    std.debug.assert(@intFromEnum(Priority.idle) == c.OVE_PRIO_IDLE);
    std.debug.assert(@intFromEnum(Priority.low) == c.OVE_PRIO_LOW);
    std.debug.assert(@intFromEnum(Priority.below_normal) == c.OVE_PRIO_BELOW_NORMAL);
    std.debug.assert(@intFromEnum(Priority.normal) == c.OVE_PRIO_NORMAL);
    std.debug.assert(@intFromEnum(Priority.above_normal) == c.OVE_PRIO_ABOVE_NORMAL);
    std.debug.assert(@intFromEnum(Priority.high) == c.OVE_PRIO_HIGH);
    std.debug.assert(@intFromEnum(Priority.realtime) == c.OVE_PRIO_REALTIME);
    std.debug.assert(@intFromEnum(Priority.critical) == c.OVE_PRIO_CRITICAL);
}

pub const Stats = struct {
    runtime_us: u64,
    cpu_percent_x100: u32,
};

/// Spawn-time configuration for a new thread.  Pass as `.{ ... }` to
/// [`Thread.spawn`]; all fields are optional.
///
/// ```zig
/// // Common case — name + priority
/// try ove.Thread(4096).spawn(allocator, .{ .name = "worker", .priority = .high },
///                             entry, .{});
/// // Defaults — anonymous, normal priority
/// try ove.Thread(4096).spawn(allocator, .{}, entry, .{});
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
    /// Scheduler priority (defaults to [`Priority.normal`]).
    priority: Priority = .normal,
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
/// `deinit`).  Workers spawned via
/// `Thread(N).spawn(allocator, cfg, entry, args)` receive a token as
/// the entry's first argument when the entry's signature declares one.
///
/// ```zig
/// fn worker(stop: ove.StopToken) void {
///     while (!stop.isStopped()) {
///         // do work
///     }
/// }
///
/// var th = try ove.Thread(4096).spawn(allocator, .{ .name = "worker" }, worker, .{});
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
        @compileError("ove.Thread.spawn: entry can take at most one non-StopToken parameter — " ++
            "the substrate's thread ABI is `void(*)(void*)`, a single pointer slot.  " ++
            "To pass multiple values, define a context struct and have the entry take " ++
            "`*Ctx`; pass `.{ &ctx }` as args.  Keep `ctx` alive until the thread exits " ++
            "(simplest: declare it at the same scope as the wrapper).  See thread.zig " ++
            "module doc for a worked example.");
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
/// Stack + substrate storage live in allocator-managed memory; the
/// substrate's `_init` path runs against them.  Works uniformly in
/// heap and zero-heap builds.
///
/// ```zig
/// var th = try ove.Thread(2048).spawn(allocator, .{ .name = "worker" }, workerEntry, .{});
/// defer th.deinit();
/// ```
pub fn Thread(comptime stack_size: usize) type {
    return struct {
        const Self = @This();

        const Backing = struct {
            stack: [stackTotal(stack_size)]u8 align(stackAlign(stack_size)),
            storage: c.ove_thread_storage_t,
        };

        allocator: std.mem.Allocator,
        handle: c.ove_thread_t,
        backing: ?*Backing, // null after `detach()` (resources leaked).

        /// Spawn a thread.  `cfg` is a [`SpawnConfig`] literal (pass
        /// `.{}` for all defaults).  `entry` may take zero, one, or two
        /// parameters; the first may be a [`StopToken`] (auto-injected
        /// by the trampoline).  `args` is a tuple matching the
        /// remaining parameters (zero or one pointer).
        pub fn spawn(
            allocator: std.mem.Allocator,
            comptime cfg: SpawnConfig,
            comptime entry: anytype,
            args: anytype,
        ) Error!Self {
            const info = comptime introspect(@TypeOf(entry), @TypeOf(args));
            const Tramp = makeTrampoline(entry, info);
            const ctx_ptr = ctxPointer(info.user_param_count, args);

            const backing = try allocator.create(Backing);
            errdefer allocator.destroy(backing);
            backing.stack = std.mem.zeroes(@TypeOf(backing.stack));
            backing.storage = std.mem.zeroes(c.ove_thread_storage_t);
            var h: c.ove_thread_t = null;
            try err.fromCode(c.ove_thread_init(
                &h,
                &backing.storage,
                comptime cfgNameZ(cfg),
                &Tramp.invoke,
                ctx_ptr,
                @intFromEnum(cfg.priority),
                stack_size,
                &backing.stack,
            ));
            return .{ .allocator = allocator, .handle = h, .backing = backing };
        }

        /// Request cooperative cancellation.  Sets the per-thread atomic
        /// stop flag.  The worker must poll [`StopToken.isStopped`] for
        /// this to have any effect — the substrate does NOT
        /// force-terminate.  Idempotent.
        pub inline fn requestStop(self: Self) void {
            if (self.handle == null) return;
            c.ove_thread_request_stop(self.handle);
        }

        pub inline fn shouldStop(self: Self) bool {
            if (self.handle == null) return false;
            return c.ove_thread_should_stop(self.handle);
        }

        pub inline fn stopToken(self: Self) StopToken {
            return .{ .handle = self.handle };
        }

        /// Consume the wrapper without waiting for the worker to exit.
        /// The underlying kernel thread keeps running and the backing
        /// stack+storage stays mapped — they're leaked from the
        /// binding's perspective.  Subsequent calls to `deinit` on a
        /// detached `Self` are no-ops.
        ///
        /// This is sound because the substrate still reads from the
        /// backing memory until the worker function returns; freeing
        /// the backing before that point would be use-after-free.
        /// Choosing to detach means accepting the leak.
        pub fn detach(self: *Self) void {
            self.handle = null;
            self.backing = null;
        }

        /// Request cooperative stop, then wait for the worker to exit
        /// and free the allocator-managed backing.  Cooperative workers
        /// (those polling [`StopToken.isStopped`]) exit cleanly.
        /// Workers that ignore the flag block here indefinitely.
        pub fn deinit(self: Self) void {
            if (self.handle != null) {
                c.ove_thread_request_stop(self.handle);
                _ = c.ove_thread_deinit(self.handle);
            }
            if (self.backing) |b| self.allocator.destroy(b);
        }

        pub fn setPriority(self: Self, priority: Priority) void {
            c.ove_thread_set_priority(self.handle, @intFromEnum(priority));
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
