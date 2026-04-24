# oveRTOS

[![tests](https://github.com/Varcain/oveRTOS/actions/workflows/ove-tests.yml/badge.svg)](https://github.com/Varcain/oveRTOS/actions/workflows/ove-tests.yml)
[![coverage](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/Varcain/2311c373541b067eeb3db3fa9580340b/raw/overtos-coverage.json)](https://github.com/Varcain/oveRTOS/actions/workflows/coverage.yml)

A portable RTOS abstraction framework that provides a unified C API across **FreeRTOS**, **Apache NuttX**, **Zephyr**, and **POSIX** (with a browser-hosted **WASM** target). Write your application once, deploy it on any supported backend with zero runtime overhead.

## Key Features

- **Write once, run on any RTOS** -- single API across four RTOS backends plus a WebAssembly target
- **Zero overhead** -- compile-time backend dispatch, no function pointers or vtables
- **Multi-language** -- C, C++, Rust, and Zig bindings
- **Flexible allocation** -- heap mode (`_create`/`_destroy`) or zero-heap mode (`_init`/`_deinit` with static storage)
- **Rich module set** -- threads, mutexes, semaphores, queues, timers, GPIO, bus drivers (UART/SPI/I2C/I2S), audio graph engine, networking (TCP/UDP/TLS/HTTP/MQTT/HTTPD/SNTP), ML inference, filesystem, NVS, LVGL GUI, shell, logging, power management, watchdog, and more
- **Unified configuration** -- single Kconfig-based `.config` drives all backends
- **Desktop development** -- develop and test on POSIX, deploy to embedded hardware

## Supported Backends

| Backend | Target | Toolchain |
|---------|--------|-----------|
| FreeRTOS | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| NuttX | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| Zephyr | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| POSIX | Linux / macOS host | Host GCC/Clang |
| WASM | Browser (WebAssembly) | Emscripten |

## Quick Start

### Prerequisites

- Python 3 with `venv` module
- CMake
- ARM GCC toolchain (for embedded targets) -- downloaded automatically
- QEMU (for emulated targets)

### Build and Run

```bash
# 1. Load a configuration (dot-syntax: <board>.<rtos>.<app>)
make host.posix.example_c

# 2. Build (downloads RTOS sources, configures, and compiles)
make

# 3. Run
make run
```

### Interactive Configuration

```bash
make menuconfig
```

### Configuration Syntax

Configurations use dot-separated `<board>.<rtos>.<app>` syntax:

```bash
make host.posix.example_c
make qemu.freertos.example_c
make qemu.nuttx.example_rust
make stm32f746.zephyr.example_cpp
make host.posix.example_c ZEROHEAP=1    # zero-heap variant
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
| `ove_audio` | Graph-based audio engine with typed nodes |
| `ove_net` | TCP/UDP sockets, DNS, TLS, HTTP, MQTT, HTTPD, SNTP |
| `ove_infer` | ML inference engine (TensorFlow Lite Micro) |
| `ove_fs` | Virtual filesystem |
| `ove_nvs` | Non-volatile key-value storage |
| `ove_lvgl` | LVGL 9.x display integration |
| `ove_shell` | Interactive command shell |
| `ove_log` | Compile-time filtered logging |
| `ove_stream` | Byte-stream ring buffers |
| `ove_watchdog` | Hardware watchdog |
| `ove_pm` | Power management (sleep states, domains, wake sources) |
| `ove_uart` | UART serial driver |
| `ove_spi` | SPI bus master driver |
| `ove_i2c` | I2C bus master driver |
| `ove_i2s` | I2S / SAI audio bus driver |

## Zero-Heap Mode

For memory-constrained or safety-critical systems, oveRTOS supports fully static allocation.
The `_create()`/`_destroy()` API works in both heap and zero-heap mode — no `#ifdef` guards needed:

```c
ove_queue_t q;
ove_mutex_t m;

ove_queue_create(&q, sizeof(uint32_t), 8);   /* heap: malloc, zero-heap: static storage */
ove_mutex_create(&m);
/* ... */
ove_mutex_destroy(m);
ove_queue_destroy(q);
```

File-scope declarations with auto-init are also available via `OVE_*_DEFINE_STATIC()`:

```c
OVE_QUEUE_DEFINE_STATIC(my_queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(my_mutex);
OVE_THREAD_DEFINE_STATIC(my_thread, 4096, worker_fn, NULL, OVE_PRIO_NORMAL, "worker");
```

Enable with `CONFIG_OVE_ZERO_HEAP=y` in your configuration.

## Debugging

Code-level debugging is available on both the QEMU and WASM targets:

- **QEMU**: the browser dashboard includes a Debug window (Monaco source view, breakpoints, step/pause/continue, call stack, registers) driven by `arm-none-eabi-gdb` attached to QEMU's GDB stub.
- **WASM**: build with `-DOVE_DEBUG=ON` to embed DWARF, install the [C/C++ DevTools Support (DWARF)](https://chromewebstore.google.com/detail/cc-devtools-support-dwarf/pdcpmagijalfljmkmjngeonclgbbannb) Chrome extension, press F12 → Sources panel, set breakpoints in your `.c` files. Optional `-DOVE_WASM_SAFE=ON` adds `SAFE_HEAP`/`ASSERTIONS`/stack-overflow-check for memory-bug hunting.

Full workflow: [Debugging](docs-site/docs/getting-started/run.md#debugging).

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
make test-nuttx                  # NuttX simulator tests
make test-zephyr                 # Zephyr native_sim tests
make test-qemu-freertos          # FreeRTOS on QEMU
make test-qemu-nuttx             # NuttX on QEMU
make test-qemu-zephyr            # Zephyr on QEMU
make test-qemu-freertos-zeroheap # FreeRTOS zero-heap on QEMU
make test-qemu-nuttx-zeroheap    # NuttX zero-heap on QEMU
make test-qemu-zephyr-zeroheap   # Zephyr zero-heap on QEMU
```

## Linting

```bash
make lint           # check-only: formatters + correctness linters
make format         # rewrite files in place (formatters only)
```

`make lint` runs the following tools, skipping any that aren't installed:

| Tool               | Scope                                         |
|--------------------|-----------------------------------------------|
| clang-format       | all `.c/.h/.cpp/.hpp` — style                 |
| clang-tidy         | app + binding C/C++ in the active compile db  |
| cargo fmt          | `bindings/rust/ove` + `apps/rust/*`           |
| cargo clippy       | same crates; `-D warnings -W pedantic -W nursery` |
| zig fmt            | all `.zig` — style                            |
| zig ast-check      | all `.zig` — parse + semantic check           |
| ruff               | `config/ove-cli` Python                       |
| backend-struct     | forbids `struct ove_*` redefinition in backend `.c` |

The `backend-struct` rule enforces a key invariant: every `ove_*_storage_t`
is an alias for the same `struct ove_X` declared in
`backends/<rtos>/include/ove_storage_<rtos>.h`. Backend `.c` files must
**never** declare `struct ove_X` at top level (narrow FS allowlist aside)
— if they do, consumers in other translation units see a different size
than the backend writes and `_init()` silently corrupts adjacent memory.
See `config/ove-cli/ove/lint_backend_struct.py` for the allowlist.

Before pushing, run `make lint && make test`. CI's first job
(`.github/workflows/ove-tests.yml::lint`) gates every downstream test
job, so a lint failure blocks the whole pipeline fast.

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
│   ├── wasm/
│   └── common/
├── bindings/           # Language bindings
│   ├── cpp/
│   ├── rust/
│   └── zig/
├── apps/               # Example applications
│   ├── c/              #   C apps: example, benchmark, example_net, example_pm, example_keyword_live, lvgl_benchmark
│   ├── cpp/            #   C++ apps: same set plus lvgl_gallery
│   ├── rust/           #   Rust apps: same set plus lvgl_gallery
│   └── zig/            #   Zig apps:  same set plus lvgl_gallery
├── models/             # ML model assets (TFLite)
├── sim/                # Simulation framework (plugins, dashboard, transports)
├── boards/             # Board definitions
├── config/             # Kconfig definitions and ove CLI
├── config/fragments/   # Configuration fragments (board, RTOS, variant)
├── tests/              # Test suites
└── docs-site/          # MkDocs documentation site
```

## License

Copyright (C) 2026 Kamil Lulko

This project is licensed under the [GNU General Public License v3.0 or later](LICENSE).

See [NOTICE](NOTICE) for third-party attribution.
