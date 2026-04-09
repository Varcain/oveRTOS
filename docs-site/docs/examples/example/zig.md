# Basic Example — Zig

Source: `apps/zig/example/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

The Zig example demonstrates the `ove` module with comptime feature detection, generic types, `defer`-based cleanup, and catch-based error handling.

## Zig patterns

Objects use Zig generics — type and size are comptime parameters:

```zig
const ove = @import("ove");
const Thread = ove.Thread;
const Queue = ove.Queue;
const Timer = ove.Timer;

var queue: Queue(u32, 8) = undefined;
var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
```

`Queue(u32, 8)` is a generic queue instantiation. `std.atomic.Value(u32)` provides lock-free shared state — same pattern as the Rust example's `AtomicU32`.

## Comptime feature detection

```zig
const has_lvgl = @hasDecl(ove.ffi, "ove_lvgl_init");
const lvgl = if (has_lvgl) ove.lvgl else undefined;

const app_title = if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_POSIX"))
    "oveRTOS(POSIX) Zig Demo"
else
    "oveRTOS Zig Demo";
```

`@hasDecl` checks at compile time whether the C FFI binding contains a symbol. This replaces `#ifdef` from C — the compiler eliminates dead code paths entirely.

## Producer thread

```zig
fn producerThread(_: ?*anyopaque) callconv(.C) void {
    var count: u32 = 0;
    ove.log.inf("Producer started", .{});

    while (true) {
        count += 1;
        queue.send(&count, 1000) catch {
            ove.log.warn("Producer: queue full, dropped {}", .{count});
        };
        Thread.sleepMs(500);
    }
}
```

Error handling uses Zig's `catch` — the send returns an error union.

## Consumer with atomic state

```zig
fn consumerThread(_: ?*anyopaque) callconv(.C) void {
    while (true) {
        var val: u32 = 0;
        queue.receive(&val, ove.WAIT_FOREVER) catch continue;
        last_value.store(val, .monotonic);

        if (val % 5 == 0) {
            ove.log.inf("Consumer: count = {}", .{val});
        }
    }
}
```

## Entry point

```zig
export fn ove_main() void {
    ove.log.inf("Zig example: init", .{});

    queue = Queue(u32, 8).init() catch return;

    _ = Thread.spawn(producerThread, null, prio.normal, "producer", 4096) catch return;
    _ = Thread.spawn(consumerThread, null, prio.normal, "consumer", 4096) catch return;

    if (has_lvgl) {
        lvgl.init() catch return;
        createUi();
        ui_timer = Timer.initPeriodic(uiTimerCb, null, 200) catch return;
        ui_timer.start() catch {};
    }

    ove.run();
}
```

## Key APIs demonstrated

| Zig API | C Equivalent | Purpose |
|---------|-------------|---------|
| `Queue(u32, 8).init()` | `ove_queue_create` | Generic typed queue |
| `Thread.spawn()` | `ove_thread_create` | Thread creation with error union |
| `Timer.initPeriodic()` | `ove_timer_create` | Periodic callback timer |
| `std.atomic.Value(u32)` | `ove_mutex_*` | Lock-free shared state |
| `@hasDecl(ove.ffi, ...)` | `#ifdef CONFIG_OVE_*` | Comptime feature detection |
| `lvgl.init()` | `ove_lvgl_init` | LVGL initialization |
| `ove.run()` | `ove_run` | Start scheduler |

## How to build

```bash
make host.posix.example_zig
make configure && make download && make && make run
```
