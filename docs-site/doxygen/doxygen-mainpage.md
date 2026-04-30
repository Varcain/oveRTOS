# oveRTOS C API Reference {#mainpage}

oveRTOS is an embedded RTOS framework that provides a unified build system,
configuration, and portable C API across FreeRTOS, Apache NuttX, Zephyr RTOS,
and POSIX. This reference documents the C API. The correct backend
implementation is selected at compile time via Kconfig
preprocessor symbols, with no virtual dispatch and [minimal runtime overhead](https://varcain.github.io/oveRTOS/benchmarks/) over the native API.

Include every module at once with the umbrella header:

```c
#include <ove/ove.h>
```

## API Modules

### Core kernel primitives

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_types | `ove/types.h` | Common types and error codes |
| @ref ove_app | `ove/app.h` | Application entry point and scheduler start |
| @ref ove_thread | `ove/thread.h` | Thread lifecycle, sleep, yield, priority |
| @ref ove_sync | `ove/sync.h` | Mutexes, semaphores, events, condition variables |
| @ref ove_queue | `ove/queue.h` | Fixed-size item FIFO message queues |
| @ref ove_timer | `ove/timer.h` | Software timers |
| @ref ove_eventgroup | `ove/eventgroup.h` | Multi-bit event flags |
| @ref ove_workqueue | `ove/workqueue.h` | Deferred work on a dedicated thread |
| @ref ove_stream | `ove/stream.h` | Byte-stream ring buffers |
| @ref ove_time | `ove/time.h` | Monotonic clock and delays |

### Board, hardware I/O, and bus drivers

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_board | `ove/board.h` | Board initialisation and identification |
| @ref ove_board_types | `ove/board_types.h` | Board descriptor and LED descriptor struct definitions |
| @ref ove_gpio | `ove/gpio.h` | General-purpose I/O |
| @ref ove_led | `ove/led.h` | On-board LED control |
| @ref ove_watchdog | `ove/watchdog.h` | Hardware watchdog timer |
| @ref ove_console | `ove/console.h` | Serial console I/O |
| @ref ove_uart | `ove/uart.h` | UART serial bus driver |
| @ref ove_spi | `ove/spi.h` | SPI master bus driver |
| @ref ove_i2c | `ove/i2c.h` | I2C master bus driver |
| @ref ove_i2s | `ove/i2s.h` | I2S / SAI audio bus driver with DMA double buffering |

### Storage, filesystem, and NVS

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_fs | `ove/fs.h` | Filesystem abstraction (VFS) |
| @ref ove_nvs | `ove/nvs.h` | Non-volatile key-value storage |

### Audio and ML

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_audio | `ove/audio.h` | Audio graph engine (sources, processors, sinks) |
| @ref ove_audio_node | `ove/audio_node.h` | Node types, vtable, and built-in processing nodes |
| @ref ove_audio_device | `ove/audio_device.h` | Transport abstraction and device node factories |
| @ref ove_infer | `ove/infer.h` | ML inference engine (LiteRT / TensorFlow Lite Micro) |

### Networking

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_net | `ove/net.h` | BSD-like sockets, DNS, network interface |
| @ref ove_net_tls | `ove/net_tls.h` | TLS sessions over TCP (mbedTLS) |
| @ref ove_net_http | `ove/net_http.h` | HTTP/1.1 client |
| @ref ove_net_mqtt | `ove/net_mqtt.h` | MQTT 3.1.1 client |
| @ref ove_net_httpd | `ove/net_httpd.h` | Embedded HTTP server with routing and WebSocket |
| @ref ove_net_sntp | `ove/net_sntp.h` | Simple NTP time synchronisation |

### Power management

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_pm | `ove/pm.h` | Sleep states, wake sources, power domains, statistics |

### User interface and diagnostics

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_shell | `ove/shell.h` | Interactive command shell |
| @ref ove_log | `ove/log.h` | Compile-time filtered logging macros |
| @ref ove_lvgl | `ove/lvgl.h` | Unified LVGL display include |
| @ref ove_lvgl_internal | `ove/lvgl_internal.h` | LVGL display integration hooks |

### Framework internals

| Module | Header | Description |
|--------|--------|-------------|
| @ref ove_umbrella | `ove/ove.h` | Umbrella header that pulls in every subsystem |
| @ref ove_storage | `ove/storage.h` | Backend-specific opaque storage types |
| @ref ove_storage_define | `ove/storage.h` | `OVE_*_DEFINE()` instance macros |
| @ref ove_storage_define_static | `ove/storage.h` | `OVE_*_DEFINE_STATIC()` file-scope declarations |
| @ref ove_storage_heap_gates | `ove/storage.h` | `OVE_HEAP_*` gates selecting heap vs. zero-heap bodies |
| @ref ove_bsp | `ove/bsp.h` | Legacy BSP compatibility shim |

## Allocation Strategies

Every object-bearing module supports two allocation modes:

- **Heap mode** (default) -- `_create()` / `_destroy()` with dynamic allocation.
- **Zero-heap mode** (`CONFIG_OVE_ZERO_HEAP`) -- `_init()` / `_deinit()` with
  caller-supplied static buffers. The `OVE_*_DEFINE_STATIC()` macros combine
  storage declaration and init into a single file-scope declaration.

## Further Documentation

For setup, build, configuration, and example walkthroughs, see the
[oveRTOS documentation site](../index.html).
