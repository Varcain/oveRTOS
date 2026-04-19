# oveRTOS C++ API Reference {#mainpage}

C++20 RAII wrappers for the oveRTOS embedded RTOS framework.
These wrappers provide type-safe, move-only handles with automatic resource
cleanup, compile-time stack sizing, and fluent LVGL widget builders.

Include every module at once with the umbrella header:

```cpp
#include <ove/ove.hpp>
```

## Wrapper Classes

### Core kernel primitives

| Class | Header | Description |
|-------|--------|-------------|
| `ove::Thread<StackSize>` | `ove/thread.hpp` | Compile-time stack-sized thread with move semantics |
| `ove::Mutex` | `ove/sync.hpp` | Non-recursive mutex |
| `ove::RecursiveMutex` | `ove/sync.hpp` | Recursive mutex |
| `ove::Semaphore` | `ove/sync.hpp` | Counting semaphore |
| `ove::Event` | `ove/sync.hpp` | Binary event flag |
| `ove::CondVar` | `ove/sync.hpp` | Condition variable |
| `ove::LockGuard` | `ove/sync.hpp` | RAII mutex lock guard |
| `ove::Queue<T, N>` | `ove/queue.hpp` | Type-safe, fixed-depth message queue |
| `ove::Timer` | `ove/timer.hpp` | Software timer |
| `ove::EventGroup` | `ove/eventgroup.hpp` | Multi-bit event flags |
| `ove::Workqueue` / `ove::Work` | `ove/workqueue.hpp` | Deferred work queue |
| `ove::Stream` | `ove/stream.hpp` | Byte-stream ring buffer |
| `ove::time::` | `ove/time.hpp` | Monotonic clock and delay helpers |

### Board, hardware I/O, and bus drivers

| Class / namespace | Header | Description |
|-------|--------|-------------|
| `ove::board::` | `ove/board.hpp` | Board lifecycle and descriptor helpers |
| `ove::gpio::` | `ove/gpio.hpp` | GPIO pin configuration and interrupt callbacks |
| `ove::led::` | `ove/led.hpp` | On-board LED helpers |
| `ove::Watchdog` | `ove/watchdog.hpp` | Hardware watchdog timer |
| `ove::console::` | `ove/console.hpp` | Serial console I/O helpers |
| `ove::Uart` | `ove/uart.hpp` | UART driver wrapper |
| `ove::Spi` | `ove/spi.hpp` | SPI driver wrapper |
| `ove::I2c` | `ove/i2c.hpp` | I2C driver wrapper |

### Storage, filesystem, and NVS

| Class / namespace | Header | Description |
|-------|--------|-------------|
| `ove::fs::File` / `ove::fs::Dir` | `ove/fs.hpp` | RAII file and directory handles |
| `ove::nvs::` | `ove/nvs.hpp` | Non-volatile key-value storage helpers |

### Audio and ML

| Class / namespace | Header | Description |
|-------|--------|-------------|
| `ove::audio::` | `ove/audio.hpp` | Audio graph engine (sources, processors, sinks) |
| `ove::infer::` | `ove/infer.hpp` | RAII wrappers for TFLite Micro model lifecycle |

### Networking

| Class / namespace | Header | Description |
|-------|--------|-------------|
| `ove::net::` | `ove/net.hpp` | Sockets, addresses, DNS helpers |
| `ove::tls::` | `ove/net_tls.hpp` | TLS session wrapper (mbedTLS) |
| `ove::http::Client` | `ove/net_http.hpp` | HTTP/1.1 client |
| `ove::mqtt::Client` | `ove/net_mqtt.hpp` | MQTT 3.1.1 client |
| `ove::httpd::` / `ove::ws::` | `ove/net_httpd.hpp` | Embedded HTTP server with WebSocket support |
| `ove::sntp::` | `ove/net_sntp.hpp` | SNTP time sync helpers |

### Power management

| Class / namespace | Header | Description |
|-------|--------|-------------|
| `ove::pm::` | `ove/pm.hpp` | Sleep states, wake sources, power domains |

### Shell and benchmarking

| Class / namespace | Header | Description |
|-------|--------|-------------|
| `ove::shell::` | `ove/shell.hpp` | Interactive shell registration helpers |
| `ove::bench::` | `ove/bench.hpp` | Common latency / throughput benchmarking utilities |

## LVGL C++ Wrappers

When `CONFIG_OVE_LVGL` is enabled, `ove/lvgl.hpp` provides:

| Class | Description |
|-------|-------------|
| `ove::lvgl::LvglGuard` | RAII guard for the LVGL display lock |
| `ove::lvgl::Component<T>` | CRTP base for composable widget components |
| `ove::lvgl::State<T>` | Reactive observable value (uses `LV_USE_OBSERVER`) |
| `ove::lvgl::Label` / `ove::lvgl::Bar` / `ove::lvgl::Box` | Fluent widget builders |

## Design Principles

- **RAII**: All kernel objects are released in destructors. No manual `_destroy()` calls needed.
- **Move-only**: Handles are non-copyable, preventing double-free.
- **Compile-time configuration**: Template parameters encode stack sizes and queue depths.
- **Zero overhead**: Wrappers inline to the underlying C API with no additional indirection.

## Further Documentation

For setup, build, configuration, and example walkthroughs, see the
[oveRTOS documentation site](../index.html).
