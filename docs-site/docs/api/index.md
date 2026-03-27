# API Reference

oveRTOS exposes its public API through a set of C headers, a C++20 wrapper layer, a Rust crate, and Zig bindings. This page maps each module to its header and gives a function count so you know where to look.

## Language references

- C API — [Doxygen Reference](c/)
- C++ API — [Doxygen Reference](cpp/)
- Rust API — [rustdoc Reference](rust/ove/)
- Zig API — [Autodoc Reference](zig/)

## Module map

Including `ove/ove.h` pulls in every subsystem listed below. Individual headers can be included separately when only a subset of the API is needed.

| Module | Header | Functions | Description |
|--------|--------|-----------|-------------|
| Types | `ove/types.h` | — | Common error codes, opaque handle typedefs, and `OVE_WAIT_FOREVER` |
| App | `ove/app.h` | 3 | Application entry point (`ove_main`), scheduler start (`ove_run`), and platform bootstrap (`ove_app_run`) |
| Thread | `ove/thread.h` | 13 | Thread lifecycle, sleep, yield, suspend/resume, state query, stack usage, runtime stats. [Guide](threads.md) |
| Sync | `ove/sync.h` | 22 | Mutex, recursive mutex, semaphore, binary event, condition variable. [Guide](sync.md) |
| Queue | `ove/queue.h` | 8 | Fixed-size item FIFO with ISR-safe variants. [Guide](ipc.md#message-queues) |
| Timer | `ove/timer.h` | 7 | Periodic and one-shot software timers. [Guide](timers.md#software-timers) |
| EventGroup | `ove/eventgroup.h` | 9 | Multi-bit event flags with ISR-safe set. [Guide](ipc.md#event-groups) |
| WorkQueue | `ove/workqueue.h` | 9 | Deferred work on a dedicated thread. [Guide](timers.md#work-queues) |
| Stream | `ove/stream.h` | 10 | Byte-stream ring buffer with trigger threshold. [Guide](ipc.md#byte-streams) |
| Audio | `ove/audio.h` `ove/audio_node.h` `ove/audio_device.h` | 15 | Graph-based audio engine with pluggable nodes and transports. [Guide](audio.md) |
| FS | `ove/fs.h` | 18 | VFS layer: mount, file I/O, directory enumeration, unlink, rename. [Guide](fs.md) |
| Console | `ove/console.h` | 4 | Serial I/O: init, getchar, putchar, write. [Guide](shell.md#console) |
| Time | `ove/time.h` | 4 | Monotonic clock: get microseconds, get nanoseconds, delay milliseconds, delay microseconds |
| Board | `ove/board.h` | 3 | Board lifecycle: init, name query, descriptor. [Guide](io.md#board) |
| GPIO | `ove/gpio.h` | 6 | Pin configure, set, get, interrupt register/enable/disable. [Guide](io.md#gpio) |
| LED | `ove/led.h` | 3 | On-board LEDs: set, toggle, count. [Guide](io.md#leds) |
| NVS | `ove/nvs.h` | 5 | Non-volatile key-value store. [Guide](nvs.md) |
| Watchdog | `ove/watchdog.h` | 7 | Hardware watchdog with feed timeout. [Guide](io.md#watchdog) |
| Shell | `ove/shell.h` | 3 | Interactive CLI: init, register command, process character. [Guide](shell.md) |
| Infer | `ove/infer.h` | 8 | ML inference engine for TFLite Micro models. [Guide](infer.md) |
| Log | `ove/log.h` | — | Compile-time filtered macros: `OVE_LOG_ERR`, `OVE_LOG_WRN`, `OVE_LOG_INF`, `OVE_LOG_DBG`, and `OVE_LOG` |
| LVGL | `ove/lvgl.h` | — | Unified LVGL include: abstraction API (`lvgl_internal.h`) plus upstream LVGL library headers |
| LVGL Internal | `ove/lvgl_internal.h` | — | Internal LVGL display integration hooks (lock/unlock/tick/handler/init) |
| Storage | `ove/storage.h` | — | Backend-specific opaque storage types (`ove_*_storage_t`), `OVE_*_DEFINE()`, and `OVE_*_DEFINE_STATIC()` macros |
| BSP | `ove/bsp.h` | — | Legacy BSP compatibility shim for older board support packages |

## Allocation strategies

`_create()` / `_destroy()` are the **primary API** and work regardless of the `CONFIG_OVE_ZERO_HEAP` setting:

- **Heap mode** (default) — `_create()` allocates from the RTOS heap.
- **Zero-heap mode** (`CONFIG_OVE_ZERO_HEAP=y`) — `_create()` becomes a GCC statement-expression macro that auto-generates per-call-site static storage and calls `_init()`. Size parameters (queue `item_size`/`max_items`, stream size, thread `stack_sz`) must be compile-time constants, and each call site produces exactly one static object.

`_init()` / `_deinit()` remain available for **explicit storage control** — use them when objects live in arrays, loops, or structs where per-call-site macro expansion is not appropriate.

The `OVE_*_DEFINE_STATIC()` macros (e.g. `OVE_QUEUE_DEFINE_STATIC`, `OVE_THREAD_DEFINE_STATIC`) combine storage declaration and init into a single declaration at file scope, and work in both modes.

## Error codes

All functions that can fail return `int`. A return value of `OVE_OK` (0) indicates success; negative values are errors:

| Constant | Value | Meaning |
|----------|-------|---------|
| `OVE_OK` | 0 | Success |
| `OVE_ERR_NOT_REGISTERED` | -1 | Backend not registered |
| `OVE_ERR_INVALID_PARAM` | -2 | Invalid argument |
| `OVE_ERR_NO_MEMORY` | -3 | Heap exhausted or unavailable |
| `OVE_ERR_TIMEOUT` | -4 | Deadline expired |
| `OVE_ERR_NOT_SUPPORTED` | -5 | Feature not supported by backend |
| `OVE_ERR_QUEUE_FULL` | -6 | Queue at maximum capacity |
