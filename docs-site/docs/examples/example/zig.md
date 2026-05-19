# Basic Example — Zig

Source: `apps/zig/heap/example/src/main.zig` (also `apps/zig/zeroheap/example/src/main.zig`) | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

The Zig example demonstrates the `ove` module with allocator-aware constructors, `comptime`-typed wrappers, exhaustive RTOS dispatch, narrow per-op error sets, and `std.log` integration.

## Allocator-aware, one API across modes

Every primitive takes a `std.mem.Allocator`. In heap mode it's `std.heap.page_allocator`; in zero-heap mode it's a `FixedBufferAllocator` over a static BSS arena. Wrapper methods are identical in both modes — only the allocator differs.

```zig
const std = @import("std");
const ove = @import("ove");

// Route std.log.* through the oveRTOS console.
pub const std_options: std.Options = .{ .logFn = ove.log.logFn };

// Page allocator for heap-mode builds. Substrate's libc-malloc heap
// and allocator-managed storage stay separate for clean accounting.
const app_allocator = std.heap.page_allocator;

// Heap mode keeps wrappers as optionals — populated in appMain, then
// unwrapped with `.?` from thread entries (which can't capture closures).
var queue: ?ove.Queue(u32, 8) = null;
var ui_timer: ?ove.Timer = null;
var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
```

`ove.Queue(u32, 8)` is a generic queue type — element type and capacity are comptime parameters. `std.atomic.Value(u32)` provides lock-free shared state — same pattern as the Rust example's `AtomicU32`.

A debug-only address tracker inside each wrapper records `&self` at `create()` and panics if any subsequent method call sees a different address. The check is compiled out in `ReleaseFast` / `ReleaseSmall`.

## RTOS detection — typed enum, exhaustive switch

`ove.target.current_rtos` is a `Rtos` enum resolved at comptime. The compiler enforces exhaustive switches; adding a new RTOS to the substrate fails every consuming switch until updated:

```zig
const app_title = switch (ove.target.current_rtos) {
    .freertos => "oveRTOS(FreeRTOS) Zig Demo",
    .nuttx => "oveRTOS(NuttX) Zig Demo",
    .zephyr => "oveRTOS(Zephyr) Zig Demo",
    .posix => "oveRTOS(POSIX) Zig Demo",
    .wasm => "oveRTOS(wasm) Zig Demo",
};
```

This replaces brittle `@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_FREERTOS")` string checks — string typos used to compile silently.

## Producer — typed `Duration`, narrow error set

`q.sendFor(item, duration)` takes a `Duration` (constructed with `.millis(n)`, `.secs(n)`, `.micros(n)`, etc.) and returns the narrow error set `error{ QueueFull, Timeout }`. Exhaustive `switch` arms catch every reachable case:

```zig
fn producerEntry() void {
    std.log.info("Producer started", .{});
    var count: u32 = 0;

    while (true) {
        count += 1;
        queue.?.sendFor(&count, .millis(1000)) catch |e| switch (e) {
            error.Timeout => std.log.warn("Producer: send timeout", .{}),
            error.QueueFull => std.log.warn("Producer: queue full, dropped {d}", .{count}),
        };
        ove.thread.sleepMs(500);
    }
}
```

## Consumer — forever recv is infallible

The forever-blocking `q.recv()` returns `T` directly (no error union). Backend programming-bug errors (invalid handle, etc.) panic at the FFI boundary instead of leaking into typed return paths:

```zig
fn consumerEntry() void {
    std.log.info("Consumer started", .{});
    while (true) {
        const val = queue.?.recv();         // infallible → u32
        last_value.store(val, .release);
        if (val % 5 == 0) {
            std.log.info("Consumer: count = {d}", .{val});
        }
    }
}
```

## Entry point — `Thread.spawn` with anytype + args

`Thread(N).spawn(allocator, config, entry, args)` mirrors `std.Thread.spawn`'s shape. `entry` is `comptime anytype` (any function reference), `args` is a tuple of run-time arguments forwarded to the entry function:

```zig
fn appMain() void {
    std.log.info("Zig example (heap mode): init", .{});

    queue = ove.Queue(u32, 8).create(app_allocator) catch return;

    _ = ove.Thread(4096).spawn(
        app_allocator,
        .{ .name = "producer", .priority = .normal },
        producerEntry, .{},
    ) catch return;

    _ = ove.Thread(4096).spawn(
        app_allocator,
        .{ .name = "consumer", .priority = .normal },
        consumerEntry, .{},
    ) catch return;

    ui_timer = ove.Timer.create(
        app_allocator,
        .{ .period_ms = 200, .mode = .periodic },
        uiTimerCallback, .{},
    ) catch return;

    ove.lvgl.init() catch return;
    {
        const guard = ove.lvgl.lock();
        defer guard.deinit();
        createUi();
    }
    ui_timer.?.start() catch return;

    ove.run();
}

comptime { ove.exportMain(appMain); }
```

`Priority` is an `enum(c_uint)` whose variants pin against the substrate's `OVE_PRIO_*` C enum at comptime — use the enum-literal `.normal` / `.high` instead of the older `ove.thread.prio.normal` constants struct.

## Zero-heap variant — same call sites, FBA allocator

The zero-heap version of this app lives at `apps/zig/zeroheap/example/src/main.zig`. Every `create(allocator)` call is identical to heap mode; only the allocator changes:

```zig
// Static-backed BSS arena routes every byte to caller-owned memory.
var arena_bytes: [4096]u8 = undefined;
var fba: std.heap.FixedBufferAllocator = undefined;

// Wrappers as `undefined` storage at file scope (movable in heap mode,
// pinned by the debug tracker in zero-heap).
var queue: ove.Queue(u32, 8) = undefined;
var producer_th: ove.Thread(4096) = undefined;

fn appMain() void {
    fba = std.heap.FixedBufferAllocator.init(&arena_bytes);
    const allocator = fba.allocator();

    queue = ove.Queue(u32, 8).create(allocator) catch return;
    producer_th = ove.Thread(4096).spawn(
        allocator,
        .{ .name = "producer", .priority = .normal },
        producerEntry, .{},
    ) catch return;

    ove.run();
}
```

`ove.allocators.c_allocator`, `page_allocator`, and `GeneralPurposeAllocator(.{})` become `@compileError` under `CONFIG_OVE_ZERO_HEAP=y` so an accidental dynamic-allocator import fails at build time, not at runtime.

## Logging — std.log via `ove.log.logFn`

Declare the `std_options` once at file scope and every `std.log.*` call (plus any library using `std.log.scoped(.tag).info(...)`) routes through the oveRTOS console:

```zig
pub const std_options: std.Options = .{ .logFn = ove.log.logFn };

const log = std.log.scoped(.producer);
log.info("queue at {*}", .{&queue});
```

## `std.io` integration

`Stream(N).reader()` / `Stream(N).writer()` return `std.io.GenericReader` / `std.io.GenericWriter`. Same for `fs.File`. Compose with every std-shaped reader chain:

```zig
const stream = try ove.Stream(256).create(allocator, .{});
const w = stream.writer();
try std.fmt.format(w, "value = {d}\n", .{42});
```

## Key APIs demonstrated

| Zig API | C Equivalent | Purpose |
|---------|-------------|---------|
| `ove.Queue(T, N).create(alloc)` | `ove_queue_init` | Generic typed queue, allocator-aware |
| `ove.Thread(N).spawn(alloc, cfg, entry, args)` | `ove_thread_init` | Templated thread, anytype entry |
| `ove.Timer.create(alloc, cfg, cb, args)` | `ove_timer_init` | Periodic / one-shot timer |
| `Duration` (`.millis`/`.secs`/...) | `uint64_t timeout_ns` | Typed timeouts |
| `Instant` + `.forever` | `OVE_WAIT_FOREVER` | Typed deadlines |
| `.sendFor(item, .millis(N))` | `ove_queue_send(..., timeout_ns)` | Bounded wait |
| `.recv()` (infallible) | `ove_queue_receive(..., OVE_WAIT_FOREVER)` | Forever wait |
| `error{ QueueFull, Timeout }` | `int rc` | Narrow per-op error sets |
| `std.log.info(...)` | `OVE_LOG_INF` | std.log routing via `ove.log.logFn` |
| `ove.target.current_rtos` | `#ifdef CONFIG_OVE_RTOS_*` | Typed RTOS enum, exhaustive switch |
| `std.atomic.Value(u32)` | (lock-free) | Lock-free shared state |
| `ove.thread.sleepMs(n)` | `ove_thread_sleep_ms(n)` | Sleep current thread |
| `ove.lvgl.init()` | `ove_lvgl_init` | LVGL bring-up |
| `ove.run()` | `ove_run` | Start scheduler |
| `ove.exportMain(fn)` | (entry-point shim) | Wire Zig fn as `ove_main` |

## How to build

```bash
make host.posix.example_zig         # heap mode
make host.posix.example_zig_zh      # zero-heap mode
make configure && make download && make && make run
```
