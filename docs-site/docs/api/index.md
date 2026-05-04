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
| Containers | (per-binding) | — | General-purpose fixed-capacity vectors, strings, and hashmaps for C++ (ETL), Rust (heapless), and Zig (hybrid). [Guide](containers.md) |
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
| Net | `ove/net.h` | 18 | BSD-like sockets, network interface, DNS resolution. [Guide](net.md) |
| Net HTTP | `ove/net_http.h` | 9 | HTTP/1.1 client (GET/POST/PUT/DELETE/PATCH). [Guide](net.md#http-client) |
| Net TLS | `ove/net_tls.h` | 8 | TLS sessions over TCP (mbedTLS). [Guide](net.md#tls) |
| Net MQTT | `ove/net_mqtt.h` | 10 | MQTT 3.1.1 client with QoS 0/1. [Guide](net.md#mqtt-client) |
| Net HTTPD | `ove/net_httpd.h` | 15+ | Embedded HTTP server with routing and WebSocket. [Guide](net.md#http-server) |
| Net SNTP | `ove/net_sntp.h` | 4 | Simple NTP time sync. [Guide](net.md#sntp) |
| PM | `ove/pm.h` | 18 | Power management: sleep states, wake sources, power domains, statistics. [Guide](pm.md) |
| UART | `ove/uart.h` | 7 | UART serial bus driver with interrupt RX buffering and thread-safe TX. [Guide](uart.md) |
| SPI | `ove/spi.h` | 6 | SPI master driver with configurable clock/mode and GPIO chip-select. [Guide](spi.md) |
| I2C | `ove/i2c.h` | 6 | I2C master driver with probe and register-level convenience APIs. [Guide](i2c.md) |
| I2S | `ove/i2s.h` | 13 | I2S / SAI audio bus driver with DMA double-buffered streaming. [Guide](i2s.md) |
| Log | `ove/log.h` | — | Compile-time filtered macros: `OVE_LOG_ERR`, `OVE_LOG_WRN`, `OVE_LOG_INF`, `OVE_LOG_DBG`, and `OVE_LOG` |
| LVGL | `ove/lvgl.h` | — | Unified LVGL include: abstraction API (`lvgl_internal.h`) plus upstream LVGL library headers |
| LVGL Internal | `ove/lvgl_internal.h` | — | Internal LVGL display integration hooks (lock/unlock/tick/handler/init) |
| Storage | `ove/storage.h` | — | Backend-specific opaque storage types (`ove_*_storage_t`), `OVE_*_DEFINE()`, and `OVE_*_DEFINE_STATIC()` macros |
| BSP | `ove/bsp.h` | — | Legacy BSP compatibility shim for older board support packages |

## Allocation strategies

oveRTOS exposes two distinct allocation APIs, sharing the same `_init()` foundation:

- **Heap mode** (default) — `_create()` / `_destroy()` allocate and free from the RTOS heap. These declarations are gated behind `OVE_HEAP_*` macros and are unavailable when `CONFIG_OVE_ZERO_HEAP=y`.
- **Zero-heap mode** (`CONFIG_OVE_ZERO_HEAP=y`) — `_create()` / `_destroy()` are not declared. Apps use `_init()` / `_deinit()` with caller-supplied storage, or `OVE_*_DEFINE_STATIC()` for file-scope objects. Calling a `_create()` symbol in a zero-heap build is a link error.

`_init()` / `_deinit()` work in both modes and are the right choice for objects living in arrays, loops, or structs where the caller decides storage placement.

The `OVE_*_DEFINE_STATIC()` macros (e.g. `OVE_QUEUE_DEFINE_STATIC`, `OVE_THREAD_DEFINE_STATIC`) declare a static storage object plus a handle, and register a constructor that calls `_init()` before `main()`. They expand to the same code in both modes, so file-scope objects compile portably across the heap/zero-heap split. Size parameters (`item_size`, `max_items`, stream size, stack size) must be compile-time constants.

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
| `OVE_ERR_NET_REFUSED` | -8 | Connection refused by remote host |
| `OVE_ERR_NET_UNREACHABLE` | -9 | Network or host unreachable |
| `OVE_ERR_NET_ADDR_IN_USE` | -10 | Local address already bound |
| `OVE_ERR_NET_RESET` | -11 | Connection reset by peer |
| `OVE_ERR_NET_DNS_FAIL` | -12 | DNS name resolution failed |
| `OVE_ERR_NET_CLOSED` | -13 | Connection closed by peer |
