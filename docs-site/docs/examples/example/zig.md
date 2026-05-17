# Basic Example — Zig

Source: `apps/zig/example/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

The Zig example demonstrates the `ove` module with comptime feature detection, embedded-storage RTOS wrappers, `defer`-based cleanup, and catch-based error handling.

## Zig patterns

The oveRTOS Zig binding uses an embedded-storage two-phase init pattern. Each wrapper is declared `undefined`, brought to life with `init()`, and torn down with `deinit()`:

```zig
const ove = @import("ove");

var queue: ove.Queue(u32, 8) = undefined;
var queue_in: bool = false;
var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);
```

`ove.Queue(u32, 8)` is a generic queue type — element type and capacity are comptime parameters. `std.atomic.Value(u32)` provides lock-free shared state — same pattern as the Rust example's `AtomicU32`.

### Why two-phase, not value-returning?

Under `CONFIG_OVE_ZERO_HEAP=y` each wrapper embeds the kernel-object storage as a struct field, and the kernel handle written by `init()` references `&self.storage` directly. Returning a wrapper by value would copy the bytes and invalidate the kernel pointer. The two-phase form keeps `self` at a stable address from `init()` through `deinit()`.

A debug-only address tracker inside each wrapper records `&self` at `init()` and panics if any subsequent method call sees a different address. The check is compiled out in `ReleaseFast` / `ReleaseSmall` / `ReleaseSafe`.

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
fn producerThread() void {
    var count: u32 = 0;
    ove.log.inf("Producer started", .{});

    while (true) {
        count += 1;
        queue.send(&count, 1000 * std.time.ns_per_ms) catch {
            ove.log.warn("Producer: queue full, dropped {}", .{count});
        };
        ove.thread.sleepMs(500);
    }
}
```

Error handling uses Zig's `catch` — `send` returns an error union.

## Consumer with atomic state

```zig
fn consumerThread() void {
    while (true) {
        const val = queue.receive(ove.wait_forever) catch continue;
        last_value.store(val, .monotonic);

        if (val % 5 == 0) {
            ove.log.inf("Consumer: count = {}", .{val});
        }
    }
}
```

## Entry point

```zig
fn appMain() void {
    ove.log.inf("Zig example: init", .{});

    queue = undefined;
    queue.init() catch return;
    queue_in = true;

    var producer: ove.Thread(4096) = undefined;
    producer.init("producer", producerThread, ove.thread.prio.normal) catch return;

    var consumer: ove.Thread(4096) = undefined;
    consumer.init("consumer", consumerThread, ove.thread.prio.normal) catch return;

    if (has_lvgl) {
        lvgl.init() catch return;
        createUi();
        ui_timer = undefined;
        ui_timer.init(uiTimerCb, 200, .periodic) catch return;
        ui_timer.start() catch {};
    }

    ove.run();
}

comptime { ove.exportMain(appMain); }
```

`ove.Thread(N)` is a function that returns a templated thread type with `N`-byte stack. The wrapper embeds the stack as a struct field under `CONFIG_OVE_ZERO_HEAP=y`; in heap mode the field is zero-sized.

## Key APIs demonstrated

| Zig API | C Equivalent | Purpose |
|---------|-------------|---------|
| `ove.Queue(u32, 8)` + `init()` | `ove_queue_create` | Generic typed queue with embedded buffer |
| `ove.Thread(4096)` + `init()` | `ove_thread_create` | Templated thread; stack lives in the wrapper |
| `ove.Timer` + `init(cb, ms, .periodic)` | `ove_timer_create` | Periodic callback timer |
| `std.atomic.Value(u32)` | (lock-free) | Lock-free shared state |
| `@hasDecl(ove.ffi, ...)` | `#ifdef CONFIG_OVE_*` | Comptime feature detection |
| `ove.thread.sleepMs(n)` | `ove_thread_sleep_ms(n)` | Sleep current thread |
| `ove.thread.yieldCpu()` | `ove_thread_yield()` | Yield time slice |
| `lvgl.init()` | `ove_lvgl_init` | LVGL initialization |
| `ove.run()` | `ove_run` | Start scheduler |
| `ove.exportMain(fn)` | (entry-point shim) | Wire Zig fn as `ove_main` |

## How to build

```bash
make host.posix.example_zig
make configure && make download && make && make run
```
