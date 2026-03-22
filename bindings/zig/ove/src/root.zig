// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig SDK — safe, idiomatic wrappers for the oveRTOS embedded RTOS framework.
//!
//! This module re-exports every oveRTOS subsystem as a typed Zig API.
//! Application code should import the package as:
//!
//! ```zig
//! const ove = @import("ove");
//! ```
//!
//! ## Key modules
//!
//! | Module | Description |
//! |--------|-------------|
//! | `Thread` | Thread creation, sleep, yield, priority management |
//! | `Mutex`, `Semaphore`, `Event`, `CondVar` | Synchronisation primitives |
//! | `Queue(T, N)` | Type-safe, capacity-bounded message queue |
//! | `Timer` | Software timers with callbacks |
//! | `EventGroup` | Multi-bit event flags for task synchronisation |
//! | `Workqueue` / `Work` | Deferred work on a dedicated thread |
//! | `Stream` | Variable-length byte-stream ring buffer |
//! | `Watchdog` | Hardware watchdog timer |
//! | `gpio`, `led`, `board` | Hardware I/O and board identification |
//! | `audio`, `fs`, `nvs` | Audio streaming, filesystem, non-volatile storage |
//! | `console`, `log` | Console I/O and formatted logging |
//! | `lvgl` | LVGL UI toolkit bindings |
//!
//! ## Heap and zero-heap modes
//!
//! Both allocation modes are supported transparently — the same API works
//! in either configuration.  When `CONFIG_OVE_ZERO_HEAP` is set, `create()`
//! functions use comptime-unique static storage instead of heap allocation.
//!
//! ## Entry point
//!
//! Export your application entry point with:
//!
//! ```zig
//! comptime { ove.exportMain(appMain); }
//! ```

/// Raw C FFI symbols from the oveRTOS C layer. Prefer the typed wrappers above this.
pub const ffi = @import("c.zig").raw;

/// Error type and conversion utilities for the oveRTOS C error codes.
pub const err = @import("error.zig");
/// Zig error set representing all possible oveRTOS failure codes.
pub const Error = err.Error;
/// Sentinel value for `timeout_ms` parameters meaning "block indefinitely".
pub const wait_forever = err.wait_forever;

/// Synchronization primitives: Mutex, RecursiveMutex, Semaphore, Event, CondVar.
pub const sync = @import("sync.zig");
/// Non-recursive mutual exclusion lock.
pub const Mutex = sync.Mutex;
/// Recursive mutual exclusion lock (same thread may lock multiple times).
pub const RecursiveMutex = sync.RecursiveMutex;
/// Counting semaphore for signalling between tasks or ISRs.
pub const Semaphore = sync.Semaphore;
/// Binary event flag for one-shot signalling between tasks or ISRs.
pub const Event = sync.Event;
/// Condition variable paired with a `Mutex` for producer/consumer patterns.
pub const CondVar = sync.CondVar;

/// Thread creation and lifecycle management.
pub const thread = @import("thread.zig");
/// RTOS thread handle. Create with `Thread.spawn()` or `Thread.spawnWithContext()`.
pub const Thread = thread.Thread;
/// Thread priority level (maps to `ove_prio_t`). Use the `prio.*` constants.
pub const Priority = thread.Priority;

/// Type-safe, capacity-bounded message queue. Parameterized by element type and depth.
pub const Queue = @import("queue.zig").Queue;

/// Software timer management.
pub const timer = @import("timer.zig");
/// Software timer handle. Create with `Timer.create()` or `Timer.createWithContext()`.
pub const Timer = timer.Timer;

/// Low-level console I/O (raw byte write / read).
pub const console = @import("console.zig");
/// Formatted logging backed by the oveRTOS console.
pub const log = @import("log.zig");

/// Multi-bit event group for task synchronization.
pub const eventgroup = @import("eventgroup.zig");
/// Event group handle supporting bitwise set/clear/wait operations.
pub const EventGroup = eventgroup.EventGroup;

/// Deferred work queue management.
pub const workqueue = @import("workqueue.zig");
/// Work queue handle. Submit `Work` items to run callbacks on a dedicated thread.
pub const Workqueue = workqueue.Workqueue;
/// A single deferred work item. Create with `Work.create()` and submit to a `Workqueue`.
pub const Work = workqueue.Work;

/// Variable-length byte stream buffer for inter-task data transfer.
pub const stream = @import("stream.zig");
/// Stream buffer handle. Supports blocking send/receive and ISR variants.
pub const Stream = stream.Stream;

/// Hardware watchdog timer management.
pub const watchdog = @import("watchdog.zig");
/// Watchdog handle. Must be fed periodically to prevent system reset.
pub const Watchdog = watchdog.Watchdog;

/// GPIO pin configuration and interrupt registration.
pub const gpio = @import("gpio.zig");
/// LED abstraction (on/off/toggle by index).
pub const led = @import("led.zig");
/// Board initialization and identification.
pub const board = @import("board.zig");
/// BSP compatibility shim that re-exports `board`, `gpio`, and `led`.
pub const bsp = @import("bsp.zig");

/// Time query and delay utilities (trailing underscore avoids `std.time` clash).
pub const time_ = @import("time_.zig");

/// Audio I/O initialization and control.
pub const audio = @import("audio.zig");
/// Filesystem mount, file, and directory operations.
pub const fs = @import("fs.zig");
/// Non-volatile storage key/value read and write.
pub const nvs = @import("nvs.zig");
/// Interactive shell command registration and character processing.
pub const shell = @import("shell.zig");
/// LVGL UI toolkit bindings (widgets, styles, event callbacks).
pub const lvgl = @import("lvgl.zig");

/// Init-once cell for zero-heap static allocation patterns.
pub const StaticCell = @import("static_cell.zig").StaticCell;

/// Write a message to the oveRTOS console.
pub fn print(comptime fmt: []const u8, args: anytype) void {
    log.print(fmt, args);
}

/// Start audio (if enabled) and the RTOS scheduler. Blocks forever.
pub fn run() void {
    ffi.ove_run();
}

/// Export ove_main entry point from a Zig function.
/// Must be called from a `comptime {}` block:
///     comptime { ove.exportMain(appMain); }
pub fn exportMain(comptime entry: fn () void) void {
    const S = struct {
        fn ove_main() callconv(.c) void {
            entry();
        }
    };
    @export(&S.ove_main, .{ .name = "ove_main" });
}
