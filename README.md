# oveRTOS

A portable RTOS abstraction framework that provides a unified C API across **FreeRTOS**, **Apache NuttX**, **Zephyr**, and **POSIX**. Write your application once, deploy it on any supported backend with zero runtime overhead.

## Key Features

- **Write once, run on any RTOS** -- single API across four RTOS backends
- **Zero overhead** -- compile-time backend dispatch, no function pointers or vtables
- **Multi-language** -- C, C++, Rust, and Zig bindings
- **Flexible allocation** -- heap mode (`_create`/`_destroy`) or zero-heap mode (`_init`/`_deinit` with static storage)
- **Rich module set** -- threads, mutexes, semaphores, queues, timers, GPIO, audio, filesystem, LVGL GUI, shell, logging, and more
- **Unified configuration** -- single Kconfig-based `.config` drives all backends
- **Desktop development** -- develop and test on POSIX/SDL2, deploy to embedded hardware

## Supported Backends

| Backend | Target | Toolchain |
|---------|--------|-----------|
| FreeRTOS | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| NuttX | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| Zephyr | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| POSIX | Linux / macOS host | Host GCC/Clang |

## Quick Start

### Prerequisites

- Python 3 with `venv` module
- CMake
- ARM GCC toolchain (for embedded targets) -- downloaded automatically
- QEMU (for emulated targets)

### Build and Run

```bash
# 1. Load a predefined configuration
make host_posix_example_c_defconfig

# 2. Build (downloads RTOS sources, configures, and compiles)
make

# 3. Run
make run
```

### Interactive Configuration

```bash
make menuconfig
```

### Available Defconfigs

Predefined configurations follow the pattern `<board>_<rtos>_<app>[_zeroheap]_defconfig`:

```
host_posix_example_c_defconfig
qemu_freertos_example_c_defconfig
qemu_nuttx_example_rust_defconfig
qemu_zephyr_example_cpp_defconfig
stm32f746_freertos_example_zig_defconfig
...
```

Run `make help` to see all available configurations and targets.

## Architecture

```
 ┌──────────────────────────────────────────────┐
 │              Application Code                │
 │          (C / C++ / Rust / Zig)              │
 ├──────────────────────────────────────────────┤
 │           oveRTOS Portable API               │
 │  thread | sync | queue | timer | gpio | ...  │
 ├────────┬────────┬─────────┬──────────────────┤
 │FreeRTOS│ NuttX  │ Zephyr  │      POSIX       │
 └────────┴────────┴─────────┴──────────────────┘
```

The backend is selected at configure time via Kconfig. All API calls resolve directly to backend-specific implementations at compile time.

## API Overview

```c
#include "ove/ove.h"

void ove_main(void)
{
    ove_queue_t queue;
    ove_mutex_t mutex;

    ove_queue_create(&queue, sizeof(uint32_t), 8);
    ove_mutex_create(&mutex);

    struct ove_thread_desc desc = {
        .name       = "worker",
        .entry      = worker_fn,
        .priority   = OVE_PRIO_NORMAL,
        .stack_size = 4096,
    };
    ove_thread_t thread;
    ove_thread_create(&thread, &desc);

    ove_run();
}
```

### Modules

| Module | Description |
|--------|-------------|
| `ove_thread` | Thread lifecycle, priority, sleep, yield |
| `ove_sync` | Mutex, semaphore, binary event, condition variable |
| `ove_queue` | Fixed-size FIFO message queues |
| `ove_timer` | Software timers |
| `ove_time` | Monotonic clock, delays |
| `ove_eventgroup` | Multi-bit event flags |
| `ove_workqueue` | Deferred work execution |
| `ove_console` | UART serial I/O |
| `ove_gpio` | Digital I/O |
| `ove_led` | LED control |
| `ove_audio` | I2S audio streaming |
| `ove_fs` | Virtual filesystem |
| `ove_nvs` | Non-volatile key-value storage |
| `ove_lvgl` | LVGL 9.x display integration |
| `ove_shell` | Interactive command shell |
| `ove_log` | Compile-time filtered logging |
| `ove_stream` | Byte-stream ring buffers |
| `ove_watchdog` | Hardware watchdog |

## Zero-Heap Mode

For memory-constrained or safety-critical systems, oveRTOS supports fully static allocation:

```c
/* Statically allocate all storage at file scope */
OVE_QUEUE_DEFINE_STATIC(my_queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(my_mutex);
OVE_THREAD_DEFINE_STATIC(my_thread, 4096, worker_fn, NULL, OVE_PRIO_NORMAL, "worker");
```

Enable with `CONFIG_OVE_ZERO_HEAP=y` in your configuration.

## Testing

```bash
make test              # Simulator tests (stub, C++, Rust, Zig, NuttX, Zephyr)
make test-qemu         # All QEMU ARM tests
make test-all          # Everything
```

Individual test suites:

```bash
make test-stub                   # Stub backend API tests
make test-cpp                    # C++ binding tests
make test-rust                   # Rust binding tests
make test-zig                    # Zig binding tests
make test-qemu-freertos          # FreeRTOS on QEMU
make test-qemu-nuttx             # NuttX on QEMU
make test-qemu-zephyr            # Zephyr on QEMU
make test-qemu-freertos-zeroheap # FreeRTOS zero-heap on QEMU
```

## Documentation

```bash
make docs          # Build complete documentation site (C, C++, Rust, Zig API + guides)
make docs-serve    # Build and serve locally at http://localhost:8000
```

## Project Structure

```
oveRTOS/
├── include/ove/        # Public API headers
├── src/                # Core framework implementation
├── backends/           # Backend implementations
│   ├── freertos/
│   ├── nuttx/
│   ├── zephyr/
│   ├── posix/
│   └── common/
├── bindings/           # Language bindings
│   ├── cpp/
│   ├── rust/
│   └── zig/
├── apps/               # Example applications
│   ├── example_c/
│   ├── example_cpp/
│   ├── example_rust/
│   ├── example_zig/
│   └── benchmark/
├── boards/             # Board definitions
├── config/             # Kconfig definitions and ove CLI
├── defconfigs/         # Predefined configurations
├── tests/              # Test suites
├── docs/               # Documentation sources
└── docs-site/          # MkDocs documentation site
```

## License

Copyright (C) 2026 Kamil Lulko

This project is licensed under the [GNU General Public License v3.0 or later](LICENSE).

See [NOTICE](NOTICE) for third-party attribution.
