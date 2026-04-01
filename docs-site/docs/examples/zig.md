# Zig Example

Source: `apps/zig/example/src/main.zig`

The Zig example demonstrates the oveRTOS Zig bindings. It implements the same producer-consumer pattern as the other examples, using Zig's comptime system to conditionally include LVGL code, idiomatic error handling with `catch`, and defer-based RAII for resource cleanup.

## Imports and comptime LVGL detection

```zig
const std = @import("std");
const ove = @import("ove");
const Thread = ove.Thread;
const Queue = ove.Queue;
const Timer = ove.Timer;
const prio = ove.thread.prio;

const has_lvgl = @hasDecl(ove.ffi, "ove_lvgl_init");
const lvgl = if (has_lvgl) ove.lvgl else undefined;
```

`@hasDecl` checks at compile time whether `ove_lvgl_init` exists in the imported C FFI namespace. This avoids any runtime overhead: when LVGL is absent the `lvgl` constant is `undefined` and all code guarded by `if (has_lvgl)` is eliminated by the compiler.

The backend is similarly detected at comptime for the title string:

```zig
const app_title = if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_FREERTOS"))
    "oveRTOS(FreeRTOS) Zig Demo"
else if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_NUTTX"))
    "oveRTOS(NuttX) Zig Demo"
else if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_ZEPHYR"))
    "oveRTOS(Zephyr) Zig Demo"
else if (@hasDecl(ove.ffi, "CONFIG_OVE_RTOS_POSIX"))
    "oveRTOS(POSIX) Zig Demo"
else
    "oveRTOS Zig Demo";
```

## Shared state

```zig
var queue: Queue(u32, 8) = undefined;
var last_value: std.atomic.Value(u32) = std.atomic.Value(u32).init(0);

var counter_label: if (has_lvgl) lvgl.Label else void = ...;
var bar: if (has_lvgl) lvgl.Bar else void = ...;
var ui_timer: Timer = undefined;
```

`Queue(u32, 8)` is a generic type parameterised at comptime by the item type and depth. `last_value` is an atomic to ensure thread-safe access between the consumer and timer callback without requiring a mutex. The conditional types for `counter_label` and `bar` collapse to `void` when LVGL is disabled, occupying zero bytes.

## Producer and consumer threads

```zig
fn producerEntry() void {
    ove.log.inf("Producer started", .{});
    var count: u32 = 0;

    while (true) {
        count += 1;
        queue.send(&count, 1000) catch |e| {
            switch (e) {
                error.Timeout   => ove.log.wrn("Producer: send timeout", .{}),
                error.QueueFull => ove.log.wrn("Producer: queue full, dropped {d}", .{count}),
                else            => ove.log.err("Producer: unexpected send error", .{}),
            }
            Thread.sleepMs(500);
            continue;
        };
        Thread.sleepMs(500);
    }
}

fn consumerEntry() void {
    while (true) {
        const val = queue.receive(ove.wait_forever) catch {
            ove.log.err("Consumer: receive error", .{});
            continue;
        };

        last_value.store(val, .release);
        if (val % 5 == 0) {
            ove.log.inf("Consumer: count = {d}", .{val});
        }
    }
}
```

Error handling uses Zig's `catch` syntax. `queue.send` and `queue.receive` return `anyerror!void` and `anyerror!T` respectively. The `switch (e)` exhaustively handles `error.Timeout` and `error.QueueFull`, with `else` catching any future additions.

## LVGL guard with defer

```zig
fn uiTimerCallback() void {
    if (!has_lvgl) return;

    const val = last_value.load(.acquire);
    const guard = lvgl.lock();
    defer guard.deinit();   // released on scope exit

    var buf: [24]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, "Count: {d}\x00", .{val}) catch return;
    _ = text;
    _ = counter_label.text(@ptrCast(&buf));
    _ = bar.value(@intCast(val % 101));
}
```

`defer guard.deinit()` ensures the LVGL lock is released when the function returns, regardless of how it exits. `std.fmt.bufPrint` formats into a stack buffer; the null terminator `\x00` is appended inline to produce a valid C string for LVGL.

## App entry and comptime export

```zig
fn appMain() void {
    ove.log.inf("Zig example: init", .{});

    queue = Queue(u32, 8).create() catch {
        ove.log.err("Failed to create queue", .{});
        return;
    };

    if (has_lvgl) {
        _ = Thread.spawn("graphics", graphicsEntry, prio.high, 4096) catch { ... };
    }
    _ = Thread.spawn("producer", producerEntry, prio.normal, 4096) catch { ... };
    _ = Thread.spawn("consumer", consumerEntry, prio.normal, 4096) catch { ... };

    if (has_lvgl) {
        ui_timer = Timer.create(uiTimerCallback, 200, false) catch { ... };
        lvgl.init() catch { ... };
        {
            const guard = lvgl.lock();
            defer guard.deinit();
            createUi();
        }
        ui_timer.start() catch { ... };
    }

    ove.log.inf("Zig example: ready", .{});
    ove.run();

    // Cleanup (only reached on POSIX)
    ove.log.inf("Zig example: shutdown", .{});
    if (has_lvgl) {
        ui_timer.stop() catch {};
        ui_timer.destroy();
    }
    queue.destroy();
}

comptime {
    ove.exportMain(appMain);
}
```

`ove.exportMain(appMain)` generates the `ove_main` C symbol at comptime using `@export`. The `comptime` block runs during compilation, not at runtime. `Thread.spawn` takes a function pointer of type `fn() void` — no `void *arg` parameter is needed because Zig closures or module-level state are used instead.

## Key APIs demonstrated

| API | Purpose |
|-----|---------|
| `@hasDecl(ove.ffi, "...")` | Comptime feature detection |
| `Queue(T, N)` | Generic fixed-depth queue type |
| `Queue.create()` / `Queue.send()` / `Queue.receive()` | Queue lifecycle and operations |
| `Thread.spawn()` | Spawn a thread from a plain `fn() void` |
| `Timer.create()` / `Timer.start()` | Software timer |
| `ove.log.inf` / `ove.log.wrn` / `ove.log.err` | Structured console logging with auto prefix |
| `lvgl.lock()` + `defer guard.deinit()` | RAII LVGL display guard |
| `std.fmt.bufPrint` | Stack-allocated format |
| `ove.run()` | Start the RTOS scheduler |
| `ove.exportMain(appMain)` | Export `ove_main` C entry point |

---

## Networking Example

Source: `apps/zig/example_net/src/main.zig`

The Zig networking example demonstrates comptime-safe networking wrappers with a structured test harness (TEST/PASS/FAIL tracking with results summary). DNS, TCP, UDP, HTTP (GET/POST/PUT), SNTP time sync, MQTT, and HTTPD are all tested using `ove.net`, `ove.net_http`, `ove.net_sntp`, `ove.net_mqtt`, and `ove.net_httpd`. All logging uses structured functions (`ove.log.inf`, etc.). See the [Networking API Guide](../api/net.md) for the full API.
