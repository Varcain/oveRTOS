# oveRTOS Documentation

oveRTOS is an embedded RTOS framework that provides a unified build system, Kconfig-based configuration, and portable API across FreeRTOS, Apache NuttX, Zephyr RTOS, and POSIX. The build system downloads RTOS sources and cross-compilation toolchains, generates RTOS-native configuration files from a single `.config`, and orchestrates compilation through each backend's native build system. Applications written against the oveRTOS API compile unchanged for any supported backend: the correct implementation is selected at compile time via Kconfig preprocessor symbols, with no virtual dispatch tables and no runtime overhead. The core API is written in C, with first-class bindings for C++, Rust, and Zig. LVGL is integrated as the GUI toolkit, with backend-portable display and input driver support across all targets.

## Key Features

- **Integrated build system** — downloads RTOS sources and toolchains, generates RTOS-native configuration (FreeRTOSConfig.h, Zephyr prj.conf, NuttX defconfig), and orchestrates cross-compilation via each backend's native build tools
- **35 API modules** — threads, synchronization, queues, timers, event groups, work queues, streams, audio, filesystem, GPIO, LEDs, console, logging, shell, NVS, watchdog, networking (sockets, TLS, HTTP, MQTT, HTTPD, SNTP), power management, bus drivers (UART, SPI, I2C, I2S), and more
- **4 RTOS backends** — FreeRTOS (via STM32CubeF7), Apache NuttX, Zephyr RTOS, and POSIX with browser-based sim dashboard for native host development
- **4 language bindings** — C, C++ (RAII wrappers), Rust (no_std crate), and Zig (comptime-safe wrappers)
- **LVGL integration** — the LVGL graphics library is included and built across all backends, with thread-safe locking and display driver support for hardware, QEMU, and simulated targets
- **Zero abstraction overhead** — compile-time backend dispatch via preprocessor; no vtables, no indirect calls
- **Two heap modes** — standard heap mode with `_create()`/`_destroy()` dynamic APIs, and zero-heap mode using caller-supplied static storage via `_init()`/`_deinit()`
- **Kconfig-based configuration** — familiar `menuconfig` TUI for selecting backends, boards, and individual modules

## Module Summary

| Header | Subsystem |
|---|---|
| `ove/types.h` | Common types and error codes |
| `ove/log.h` | Logging |
| `ove/thread.h` | Thread management |
| `ove/sync.h` | Mutexes, semaphores, events, condvars |
| `ove/audio.h` | Graph-based audio engine |
| `ove/fs.h` | Filesystem abstraction |
| `ove/queue.h` | Message queues |
| `ove/timer.h` | Software timers |
| `ove/console.h` | Console / serial output |
| `ove/time.h` | Monotonic clock and delays |
| `ove/board.h` | Board initialisation and identification |
| `ove/gpio.h` | General-purpose I/O |
| `ove/led.h` | On-board LED control |
| `ove/bsp.h` | Legacy BSP compatibility shim |
| `ove/lvgl.h` | Unified LVGL include (abstraction API + upstream library) |
| `ove/lvgl_internal.h` | LVGL display integration (internal helpers) |
| `ove/eventgroup.h` | Event groups (multi-bit flags) |
| `ove/workqueue.h` | Deferred work queues |
| `ove/stream.h` | Stream buffers |
| `ove/watchdog.h` | Hardware watchdog timer |
| `ove/nvs.h` | Non-volatile storage |
| `ove/shell.h` | Interactive shell |
| `ove/storage.h` | Backend-specific opaque storage types and static macros |
| `ove/net.h` | Networking: sockets, DNS, network interface |
| `ove/net_http.h` | HTTP/1.1 client |
| `ove/net_tls.h` | TLS/SSL (mbedTLS) |
| `ove/net_mqtt.h` | MQTT 3.1.1 client |
| `ove/net_httpd.h` | Embedded HTTP server |
| `ove/net_sntp.h` | SNTP time synchronization |
| `ove/uart.h` | UART serial bus driver |
| `ove/spi.h` | SPI bus master driver |
| `ove/i2c.h` | I2C bus master driver |
| `ove/i2s.h` | I2S / SAI audio bus driver |
| `ove/pm.h` | Power management framework |
| `ove/app.h` | Application lifecycle hooks |

Include every module at once with the umbrella header:

```c
#include <ove/ove.h>
```

## Quick Links

- [Getting Started — Overview](getting-started/overview.md)
- [Setup your environment](getting-started/setup.md)
- [Configure a target](getting-started/configure.md)
- [Build](getting-started/build.md)
- [Run and flash](getting-started/run.md)
- [API Reference](api/index.md)
- [Examples](examples/index.md)
