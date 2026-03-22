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
| Thread | `ove/thread.h` | 13 | Thread lifecycle (init/create/destroy/deinit), sleep, yield, suspend/resume, state query, stack usage, runtime stats, and priority change |
| Sync | `ove/sync.h` | 22 | Non-recursive mutex, recursive mutex, counting semaphore, binary event, and condition variable — each with init/deinit and heap create/destroy variants, plus lock/unlock/take/give/wait/signal/broadcast operations |
| Queue | `ove/queue.h` | 6 | Fixed-size item FIFO queue: init/deinit, heap create/destroy, send/receive (blocking and ISR-safe non-blocking variants) |
| Timer | `ove/timer.h` | 7 | Software timers: init/deinit, heap create/destroy, start, stop, and reset |
| EventGroup | `ove/eventgroup.h` | 7 | Multi-bit event flags: init/deinit, heap create/destroy, set/clear bits (including ISR-safe variant), wait-bits with `OVE_EG_WAIT_ALL`/`OVE_EG_CLEAR_ON_EXIT` flags, and get-bits |
| WorkQueue | `ove/workqueue.h` | 9 | Deferred work on a dedicated thread: queue init/deinit, heap create/destroy, work item init (static and heap), free, submit, submit-delayed, and cancel |
| Stream | `ove/stream.h` | 8 | Byte-stream ring buffer with trigger threshold: init/deinit, heap create/destroy, send/receive (task and ISR variants), reset, and bytes-available query |
| Audio | `ove/audio.h` | 6 | I2S streaming with process callback: init, start, stop, pause, resume, and deinit |
| FS | `ove/fs.h` | 14 | VFS layer: mount/unmount, open/close/read/write/seek/tell/size for files (static and heap variants), opendir/readdir/closedir, unlink, and rename |
| Console | `ove/console.h` | 4 | Serial I/O: init, getchar, putchar, and write |
| Time | `ove/time.h` | 4 | Monotonic clock: get microseconds, get nanoseconds, delay milliseconds, and delay microseconds |
| Board | `ove/board.h` | 3 | Board lifecycle: init, name query, and descriptor pointer |
| GPIO | `ove/gpio.h` | 6 | Pin configure, set, get, interrupt register, enable, and disable |
| LED | `ove/led.h` | 3 | On-board LEDs: set, toggle, and count |
| NVS | `ove/nvs.h` | 5 | Non-volatile key-value store: init, deinit, read, write, and erase |
| Watchdog | `ove/watchdog.h` | 7 | Hardware watchdog: init/deinit, heap create/destroy, start, stop, and feed |
| Shell | `ove/shell.h` | 3 | Interactive CLI: init, register command, and process character |
| Log | `ove/log.h` | — | Compile-time filtered macros: `OVE_LOG_ERR`, `OVE_LOG_WRN`, `OVE_LOG_INF`, `OVE_LOG_DBG`, and `OVE_LOG` |
| LVGL | `ove/lvgl.h` | — | Unified LVGL include: abstraction API (`lvgl_internal.h`) plus upstream LVGL library headers |
| LVGL Internal | `ove/lvgl_internal.h` | — | Internal LVGL display integration hooks (lock/unlock/tick/handler/init) |
| Storage | `ove/storage.h` | — | Backend-specific opaque storage types (`ove_*_storage_t`), `OVE_*_DEFINE()`, and `OVE_*_DEFINE_STATIC()` macros |
| BSP | `ove/bsp.h` | — | Legacy BSP compatibility shim for older board support packages |

## Allocation strategies

Every object-bearing module follows the same dual-allocation pattern:

- **Static (zero-heap)** — use `_init()` / `_deinit()` and supply your own storage. No heap allocation occurs. Required when `CONFIG_OVE_ZERO_HEAP` is set.
- **Heap** — use `_create()` / `_destroy()`. Available when `CONFIG_OVE_ZERO_HEAP` is not set. The `OVE_HEAP_*` symbols gate each heap path individually.

The `OVE_*_DEFINE_STATIC()` macros (e.g. `OVE_QUEUE_DEFINE_STATIC`, `OVE_THREAD_DEFINE_STATIC`) combine storage declaration and init into a single declaration at file scope.

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
