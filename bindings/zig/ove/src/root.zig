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
//! ## Hot-path inline discipline
//!
//! Wrapper methods that are a thin `c.ove_*(...)` plus
//! `err.fromCode(rc)` are marked `pub inline fn` so the LLVM
//! optimizer folds the FFI call, the error-code switch, and the
//! `Error!void` construction into the caller's frame.  Without this,
//! `Mutex.lock` and friends compile as out-of-line `bl` targets that
//! cost an extra ~50–200 ns per call on Cortex-M and show up as a
//! 5–15% per-binding overhead on the shortest sync paths.  Any new
//! wrapper added here should keep the body a one-liner and be marked
//! `pub inline fn` — Zig's auto-inliner is good but explicit `inline`
//! removes ambiguity at the IR level for cross-module folding.
//!
//! Cross-language inlining between Zig and C is gated behind the
//! `OVE_CROSS_LTO=ON` CMake option (declared in
//! `cmake/OveCommon.cmake`).  It is OFF by default because it
//! requires a bitcode-aware linker matching across the C and Zig
//! toolchains; per Gale's "three quiet barriers" analysis,
//! mismatched feature sets between the C and Zig sides silently
//! kill the inliner without warning.
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
//! in either configuration.  Wrappers follow a uniform two-phase pattern:
//!
//! ```zig
//! var mtx: ove.Mutex = undefined;
//! try mtx.init();
//! defer mtx.deinit();         // register only after init() succeeds
//! try mtx.lock(ove.wait_forever);
//! ```
//!
//! Under `CONFIG_OVE_ZERO_HEAP=y` each wrapper embeds the kernel-object
//! storage as a struct field; in heap mode that field is zero-sized.
//!
//! ### Pinning contract
//!
//! After `init()` the wrapper must remain at a stable address until
//! `deinit()`.  The kernel handle stored in the wrapper references
//! `&self.storage` directly, so moving, copying, or storing the wrapper
//! by value in a relocating container (e.g. `std.ArrayList`) will silently
//! corrupt RTOS state.  Debug builds (`std.debug.runtime_safety == true`)
//! record `&self` at `init()` and panic if any subsequent method sees a
//! different address.  Release builds compile the check out at zero cost.
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
/// Templated RTOS thread wrapper.  `Thread(stack_size)` returns the type;
/// declare `var th: ove.Thread(2048) = undefined;` then `try th.init(...)`.
pub const Thread = thread.Thread;
/// Thread priority level (maps to `ove_prio_t`). Use the `thread.prio.*` constants.
pub const Priority = thread.Priority;

/// Type-safe, capacity-bounded message queue. Parameterized by element type and depth.
pub const Queue = @import("queue.zig").Queue;

/// Software timer management.
pub const timer = @import("timer.zig");
/// Software timer.  Declare `var t: ove.Timer = undefined;` then
/// `try t.init(callback, period_ms, .periodic);`.
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
/// Templated work queue. `Workqueue(stack_size)` returns the type.
pub const Workqueue = workqueue.Workqueue;
/// A single deferred work item.  Declare with `undefined`, init with handler.
pub const Work = workqueue.Work;

/// Variable-length byte stream buffer for inter-task data transfer.
pub const stream = @import("stream.zig");
/// Templated stream buffer.  `Stream(byte_capacity)` returns the type.
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

/// Time query and delay utilities.
pub const time = @import("time.zig");

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

/// ML inference engine (LiteRT / TF Lite Micro).
pub const infer = @import("infer.zig");
/// ML model session.  Declare with `undefined`; `init` takes a caller-
/// supplied arena slice.
pub const Model = infer.Model;

/// BSD-like sockets, DNS resolution, and network interface management.
pub const net = @import("net.zig");
/// Generic socket address (IPv4/IPv6). Value type.
pub const Address = net.Address;
/// Network interface handle.
pub const NetIf = net.NetIf;
/// TCP stream socket.
pub const TcpStream = net.TcpStream;
/// UDP datagram socket.
pub const UdpSocket = net.UdpSocket;

/// HTTP/1.1 client.
pub const net_http = @import("net_http.zig");
/// HTTP client handle.
pub const HttpClient = net_http.Client;

/// MQTT 3.1.1 client.
pub const net_mqtt = @import("net_mqtt.zig");
/// MQTT client handle.
pub const MqttClient = net_mqtt.Client;

/// SNTP time synchronization client.
pub const net_sntp = @import("net_sntp.zig");

/// Embedded HTTP server (singleton).
pub const net_httpd = @import("net_httpd.zig");

/// TLS/SSL session wrapper (mbedTLS).
pub const net_tls = @import("net_tls.zig");
/// TLS session handle.
pub const TlsSession = net_tls.Session;

/// UART serial bus driver.
pub const uart = @import("uart.zig");
/// SPI bus master driver.
pub const spi = @import("spi.zig");
/// I2C bus master driver.
pub const i2c = @import("i2c.zig");

/// Power management framework: sleep states, wake sources, power domains.
pub const pm = @import("pm.zig");

/// Init-once cell for zero-heap static allocation patterns.
pub const StaticCell = @import("static_cell.zig").StaticCell;

/// General-purpose, fixed-capacity containers (Vec, String) and a stdlib
/// allocator helper (`fixedBufferAlloc`).  Use these for heap-free
/// resizable buffers and string builders, or to back stdlib unmanaged
/// containers (`std.ArrayListUnmanaged`, `std.AutoHashMapUnmanaged`,
/// `std.PriorityQueue`) with a static byte slice.
pub const containers = @import("containers.zig");
/// Fixed-capacity vector with comptime capacity. `Vec(T, N)` returns the type.
pub const Vec = containers.Vec;
/// Fixed-capacity, UTF-8-friendly string. `String(N)` returns the type.
pub const String = containers.String;
/// Construct a `std.heap.FixedBufferAllocator` over a caller-supplied byte slice.
pub const fixedBufferAlloc = containers.fixedBufferAlloc;

/// Write a message to the oveRTOS console.
pub fn print(comptime fmt: []const u8, args: anytype) void {
    log.print(fmt, args);
}

/// Start audio (if enabled) and the RTOS scheduler. Blocks forever.
pub fn run() void {
    ffi.ove_run();
}

/// Start the RTOS scheduler **without** engaging the zero-heap lock.
///
/// Used by apps whose runtime structurally requires post-init dynamic
/// allocation — notably the benchmark suite, which spawns helper threads
/// inside test setup paths after `ove_main` has returned.  On NuttX
/// zero-heap builds, the heap lock would cause `pthread_create`'s
/// `kmm_zalloc(task_group_s)` to fail with `ENOMEM`.  Calling this in
/// place of `run` opts out of the lock.  Never returns.
pub fn startScheduler() void {
    ffi.ove_thread_start_scheduler();
}

/// Export ove_main entry point from a Zig function.
/// Must be called from a `comptime {}` block:
///     comptime { ove.exportMain(appMain); }
///
/// Object lifetime: anything that worker threads access after `appMain`
/// returns must have static storage — declare it as a file-scope `var`
/// (BSS) or `const` (data).  Stack-local vars in `appMain` are popped
/// when it unwinds; a thread that kept a pointer into them then
/// references freed memory.  Same rule Zig enforces when you try to
/// return a pointer to a local.  On FreeRTOS the failure is immediate
/// (the scheduler reclaims the main stack); on POSIX/NuttX/Zephyr the
/// UB is latent.
pub fn exportMain(comptime entry: fn () void) void {
    const S = struct {
        fn ove_main() callconv(.c) void {
            entry();
        }
    };
    @export(&S.ove_main, .{ .name = "ove_main" });
}
