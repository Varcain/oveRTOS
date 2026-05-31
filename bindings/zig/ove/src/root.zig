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
//! | `time` | Monotonic clock, sleep, deadline helpers |
//! | `gpio`, `led`, `uart`, `i2c`, `spi`, `board`, `bsp` | Hardware I/O and board services |
//! | `audio`, `fs`, `nvs` | Audio graph, filesystem, non-volatile storage |
//! | `net`, `net_http`, `net_httpd`, `net_mqtt`, `net_sntp`, `net_tls` | Sockets, HTTP client/server, MQTT, SNTP, TLS |
//! | `shell`, `console`, `log` | Console I/O, shell command registration, `std.log` facade |
//! | `infer` | TFLite Micro inference (per-mode `Model`) |
//! | `pm` | Power management (states, wake sources, policy/notify handlers) |
//! | `lvgl` | LVGL UI toolkit bindings |
//! | `containers`, `static_cell`, `allocators`, `target` | Utility containers and runtime helpers |
//!
//! ## Known gaps
//!
//! The following oveRTOS C headers do not yet have a typed Zig wrapper —
//! callers must reach into `ove.ffi` (raw `c.ove_*` symbols) directly:
//! - `i2s.h` — I²S audio driver (no `i2s.zig`).
//! - `audio_device.h`, `audio_node.h` — only `audio.Graph` lifecycle is
//!   wrapped; device source/sink factories and individual node types
//!   require raw FFI.
//! - `storage.h` — storage backend abstraction (~1 kLOC C API).
//! - `profiler.h`, `trace.h`, `ove_arm_backtrace.h` — diagnostic /
//!   platform-specific; intentionally C-only.
//!
//! ## Heap and zero-heap modes
//!
//! Both allocation modes are supported transparently — the same API works
//! in either configuration.  Most wrappers (sync, queue, eventgroup, timer,
//! workqueue, stream, thread) follow an allocator-aware constructor pattern:
//!
//! ```zig
//! var mtx = try ove.Mutex.create(allocator);
//! defer mtx.deinit();
//! mtx.lock();
//! defer mtx.unlock();
//! ```
//!
//! The substrate-storage block lives in allocator-managed memory; the
//! wrapper itself holds an `Allocator` plus two pointers and is movable
//! by value.  Heap mode passes a libc allocator (or `GeneralPurposeAllocator`);
//! zero-heap mode passes a `FixedBufferAllocator` over a `.bss` byte buffer.
//!
//! A handful of subsystems whose handle type is forced by the C layer
//! (`Watchdog`, `NetIf`, `TcpStream`, `UdpSocket`, `Model`, `net_http.Client`,
//! `net_mqtt.Client`, `net_tls.Session`) still ship a per-mode shape:
//! heap mode is value-returning (`create()`); zero-heap mode uses
//! two-phase init (`var w: Watchdog = undefined; try w.init(...)`).
//!
//! ### Pinning contract
//!
//! Embedded-storage wrappers in the per-mode group above must remain at
//! a stable address between `init()` and `deinit()` under
//! `CONFIG_OVE_ZERO_HEAP=y` — the kernel handle references `&self.storage`
//! directly.  `.Debug` builds record `&self` at `init()` via
//! [`pin.Tracker`] and panic if a subsequent method sees a different
//! address; `.ReleaseSafe` / `.ReleaseFast` / `.ReleaseSmall` builds
//! compile the check out at zero cost (see `safety` in `pin.zig`).
//! The allocator-based wrappers do not need pinning because the storage
//! lives at the allocator's stable address; only the wrapper's two-word
//! handle moves.
//!
//! ## Entry point
//!
//! Export your application entry point with:
//!
//! ```zig
//! comptime { ove.exportMain(appMain); }
//! ```

/// True when built against a `CONFIG_OVE_ZERO_HEAP` config — the static-storage
/// mode where some types (Watchdog, infer.Model, net.*) use a different
/// (storage-backed) API.  Lets callers `comptime`-branch on the allocation mode.
pub const zero_heap = @import("pin.zig").zero_heap;

/// Raw C FFI symbols from the oveRTOS C layer. Prefer the typed wrappers above this.
pub const ffi = @import("c.zig").raw;

/// Error type and conversion utilities for the oveRTOS C error codes.
pub const err = @import("error.zig");
/// Zig error set representing all possible oveRTOS failure codes.
pub const Error = err.Error;
/// Raw nanosecond sentinel meaning "block indefinitely".  Prefer the
/// typed [`Instant.forever`] sentinel for the binding's `*Until` methods;
/// `wait_forever` is exposed for callers passing raw `u64` timeouts to
/// the C FFI layer directly.
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
/// Templated RTOS thread wrapper.  `Thread(stack_size)` returns the type.
/// Spawn via `try ove.Thread(2048).spawn(allocator, .{ .name = "worker" }, entry, .{})`;
/// works uniformly across heap and zero-heap builds.
pub const Thread = thread.Thread;
/// Thread priority level (maps to `ove_prio_t`).  See [`thread.Priority`] for
/// the enum variants (`.idle`, `.low`, `.normal`, `.high`, ...).
pub const Priority = thread.Priority;
/// Read-only handle to a thread's cooperative-cancellation flag.
/// Auto-injected into `spawn` entries whose first param is `StopToken`.
pub const StopToken = thread.StopToken;
/// Spawn-time configuration (`name`, `priority`) for [`Thread.spawn`].
/// All fields default; pass `.{}` for anonymous + normal-priority.
pub const SpawnConfig = thread.SpawnConfig;

/// Type-safe, capacity-bounded message queue.  Parameterised by element
/// type `T` and depth `N`; construct via `try Queue(T, N).create(allocator)`.
pub const Queue = @import("queue.zig").Queue;

/// Software timer management.
pub const timer = @import("timer.zig");
/// Software timer.  Create via
/// `try ove.Timer.create(allocator, .{ .period_ms = 100 }, callback, .{})`;
/// the substrate-storage lives in allocator-managed memory.
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
/// Templated work queue. `Workqueue(stack_size)` returns the type;
/// construct via `try Workqueue(N).create(allocator, name, priority)`.
pub const Workqueue = workqueue.Workqueue;
/// A single deferred work item bound to a Zig callback.  Construct via
/// `try ove.Work.create(allocator, handler)`.
pub const Work = workqueue.Work;

/// Variable-length byte stream buffer for inter-task data transfer.
pub const stream = @import("stream.zig");
/// Templated stream buffer.  `Stream(byte_capacity)` returns the type;
/// construct via `try Stream(N).create(allocator, trigger_bytes)`.
pub const Stream = stream.Stream;

/// Hardware watchdog timer management.
pub const watchdog = @import("watchdog.zig");
/// Watchdog handle.  Heap mode: `try ove.Watchdog.create(timeout_ms)`.
/// Zero-heap mode: `var w: Watchdog = undefined; try w.init(timeout_ms)`.
/// Must be fed periodically to prevent system reset.
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

/// Allocator namespace.  Wraps `std.heap.*` re-exports — static-backed
/// allocators (FixedBufferAllocator, ArenaAllocator, MemoryPool) flow
/// through verbatim; dynamic-backed ones (c_allocator, page_allocator,
/// GeneralPurposeAllocator) become `@compileError` under
/// `CONFIG_OVE_ZERO_HEAP` with a remediation message.
pub const allocators = @import("allocators.zig");

/// Compile-time target descriptors.  `ove.target.current_rtos` is a
/// typed enum value that callers `switch` over exhaustively, replacing
/// ad-hoc `@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_*")` checks.
pub const target = @import("target.zig");
/// Typed duration for the binding's bounded-wait methods
/// (`Mutex.lockFor`, `Queue.sendFor`, etc.).
pub const Duration = time.Duration;
/// Typed monotonic deadline for the binding's deadline-wait methods
/// (`Mutex.lockUntil`, `Queue.sendUntil`, etc.).
pub const Instant = time.Instant;

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
/// Socket address.  Value type.  Zig-side constructors are IPv4-only
/// (`Address.ipv4`, `Address.any`); the underlying C struct supports
/// IPv6 but the binding does not expose it yet.
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
