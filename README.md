# oveRTOS

[![tests](https://github.com/Varcain/oveRTOS/actions/workflows/ove-tests.yml/badge.svg)](https://github.com/Varcain/oveRTOS/actions/workflows/ove-tests.yml)
[![coverage](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/Varcain/2311c373541b067eeb3db3fa9580340b/raw/overtos-coverage.json)](https://github.com/Varcain/oveRTOS/actions/workflows/coverage.yml)

Write embedded RTOS applications in **C++**, **Rust**, or **Zig** — and run them unchanged on **FreeRTOS**, **Zephyr**, or **Apache NuttX**.

First-class language bindings sit on top of a unified application API: threads, synchronisation, networking, audio, ML inference, LVGL, power management. The binding you pick decides the ergonomics; the kernel underneath is a configure-time choice with [minimal runtime overhead](https://varcain.github.io/oveRTOS/benchmarks/).

A pure C API ships alongside the higher-level bindings as the substrate they share. Native POSIX and WebAssembly backends are available for development and browser-hosted demos — production targets are FreeRTOS, Zephyr, and NuttX.

> **New here?** → [**Quickstart in 5 minutes**](https://varcain.github.io/oveRTOS/getting-started/quickstart/). Hit a wall? → run `make doctor` or browse the [Troubleshooting page](https://varcain.github.io/oveRTOS/getting-started/troubleshooting/). Building an app outside the tree? → [`ove app new`](https://varcain.github.io/oveRTOS/build-system/external-apps/) stamps a working skeleton.

## Key Features

- **Modern-language application development** — C++23 (RAII + typed containers + `std::expected`-based `Result<T>` returns), Rust (`no_std` + `alloc`), Zig (`comptime`-generic), plus the underlying C surface
- **Cross-RTOS portability** — same source on FreeRTOS, Zephyr, and NuttX; backend chosen at configure time
- **Application-grade modules** — sockets / TLS / HTTP / MQTT / HTTPD / SNTP, audio graph engine, TFLite Micro inference, LVGL widgets, NVS, filesystem, power management, shell, watchdog, bus drivers (UART/SPI/I2C/I2S)
- **Flexible allocation** — heap mode (`_create` / `_destroy`, gated by `OVE_HEAP_*`) or zero-heap mode (`_init` / `_deinit` with caller-supplied storage); the static-allocation helpers (`OVE_*_DEFINE_STATIC`) work in both modes
- **[Minimal abstraction overhead](https://varcain.github.io/oveRTOS/benchmarks/)** — compile-time backend dispatch, no vtables, no indirect calls; per-op wrapper cost is benchmarked across every binding on every commit
- **Unified configuration** — a single Kconfig-based `.config` drives the binding, the kernel, and every module
- **POSIX + WebAssembly development backends** — develop on Linux/macOS without hardware, demo in a browser

## Supported deployment targets

| RTOS | Boards | Toolchain |
|------|--------|-----------|
| FreeRTOS | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| Apache NuttX | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |
| Zephyr | STM32F746G-DISCO, QEMU Cortex-M7 | ARM GCC |

## Development & demo backends

| Backend | Use | Toolchain |
|---------|-----|-----------|
| POSIX | Develop and test on Linux / macOS without hardware | Host GCC/Clang |
| WebAssembly | Same app rendered in a browser for live demos | Emscripten |

## Quick Start

### Prerequisites

- Python 3 with `venv` module
- CMake
- ARM GCC toolchain (for embedded targets) -- downloaded automatically
- QEMU (for emulated targets)

### Build and Run

```bash
# 1. Load a configuration (dot-syntax: <board>.<rtos>.<app>)
make host.posix.example_cpp

# 2. Build (downloads RTOS sources, configures, and compiles)
make

# 3. Run
make run
```

The same app in another language:

```bash
make host.posix.example_rust      # Rust
make host.posix.example_zig       # Zig
make host.posix.example_c         # plain C (the binding substrate)
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
make host.posix.example_c_zh
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

The backend is selected at configure time via Kconfig. All API calls resolve directly to backend-specific implementations at compile time — there is no runtime dispatch.

## API Overview

The same producer + worker shape expressed in each binding, in both heap mode (kernel objects from the system heap, freed via `Drop`/`deinit`/`_destroy`) and zero-heap mode (every byte from BSS, no allocator linked in). The recommended path for application code is one of the higher-level bindings (C++, Rust, Zig); the C surface below them is the shared substrate that all three compile down to. Every binding lowers to the same FFI symbols — per-call wrapper overhead is benchmarked across the matrix at [varcain.github.io/oveRTOS/benchmarks](https://varcain.github.io/oveRTOS/benchmarks/).

### C++23 (RAII, `Result<T>`, std-composable)

Built with **`-std=c++23`** across every backend. Fallible operations
return `ove::Result<T>` (an alias for `std::expected<T, ove::Error>`);
forever-blocking forms return `void` and abort on substrate failure.
`ove::Mutex` satisfies the `Lockable` named requirement so it composes
with `std::lock_guard` and `std::scoped_lock` directly.

**Heap**:

```cpp
#include <ove/ove.hpp>
using namespace std::chrono_literals;

static void worker_fn(void *) { /* ... */ }

void ove_main()
{
    ove::Queue<uint32_t, 8> queue;          // typed + bounded
    ove::Mutex mutex;                       // RAII — destructor cleans up
    ove::Thread<4096> worker(worker_fn, nullptr,
                             OVE_PRIO_NORMAL, "worker");

    // Bounded operations return Result<T> — no rc-codes to compare.
    if (auto r = queue.try_send_for(42u, 100ms); !r) {
        // r.error() is a typed ove::Error: Timeout / QueueFull / ...
    }

    // std::lock_guard composes with ove::Mutex out of the box.
    {
        std::lock_guard<ove::Mutex> g(mutex);
        /* mutex held */
    }

    ove::run();
    /* destructors run on scope exit / app shutdown */
}
```

**Zero-heap** — same wrappers, file-scope statics carry the kernel-object
storage (and thread stack) inline; constructors call `ove_*_init` with
pointers into those members during static init:

```cpp
#include <ove/ove.hpp>

static void worker_fn(void *) { /* ... */ }

static ove::Queue<uint32_t, 8> queue;
static ove::Mutex mutex;
static ove::Thread<4096> worker(worker_fn, nullptr,
                                OVE_PRIO_NORMAL, "worker");

void ove_main() { ove::run(); }
```

Move/copy are deleted on every wrapper in zero-heap mode (the kernel
holds pointers into `&storage_`), so each instance is structurally
pinned to its file-scope address.

**`Result<T>` highlights** — where the success payload is non-trivial,
the API drops out-parameters in favour of the Result value side:

```cpp
auto resp = http_client.get("http://example.com/");
if (resp) { use(resp->status(), resp->body()); }     // Result<Response>

auto sent = sock.send(buf, len);
if (sent && *sent == len) { /* fully sent */ }       // Result<size_t>

auto addr = ove::dns::resolve("example.com", 5s);    // Result<Address>
auto utc  = ove::sntp::get_utc();                    // Result<uint32_t>
```

Other std-mirror conveniences:

- `ove::this_thread::sleep_ms`/`sleep_for`/`yield`/`get_id` — mirrors `std::this_thread::*`.
- `ove::stop_token` / `ove::stop_source` — mirrors `std::stop_token` / `std::stop_source` for cooperative thread cancellation, with `std::jthread`-style capturing-lambda thread constructors.
- `ove::Error` round-trips through `std::error_code` via a bundled category.

### Rust (no_std, errors-as-values)

**Heap**:

```rust
use ove::{mutex, queue, thread, Priority};

fn worker() { /* ... */ }

fn app_main() {
    let _q = queue!(u32, 8);                // generic + comptime depth
    let _m = mutex!();                      // RAII — Drop cleans up
    let _t = thread!("worker", worker, Priority::Normal, 4096);
    ove::run();
}

ove::main!(app_main);                       // exports `ove_main` symbol
```

**Zero-heap** — `ove::shared!` parks each wrapper in a `StaticCell`; the
`ove::queue!` / `ove::thread!` macros expand to a function-scope
`static mut <storage>` plus the matching `from_static(...)` call, so the
caller-owned origin stays visible.  No `Box`, `alloc`, or operator new:

```rust
#![cfg_attr(not(feature = "std"), no_std)]
use ove::{Priority, Queue, Thread};

fn worker() { /* ... */ }

ove::shared!(QUEUE: Queue<u32, 8>);
ove::shared!(WORKER: Thread<4096>);

fn app_main() {
    QUEUE.init(ove::queue!(u32, 8));
    WORKER.init(ove::thread!("worker", worker, Priority::Normal, 4096));
    ove::run();
}

ove::main!(app_main);
```

### Zig (comptime-safe wrappers, embedded storage)

**Heap** — `Type.create(...)` returns `Error!Self`, calling `ove_*_create`
under the hood; `deinit()` is the matching destructor:

```zig
const ove = @import("ove");

fn worker() void { /* ... */ }

var queue: ?ove.Queue(u32, 8) = null;
var worker_th: ?ove.Thread(4096) = null;

fn appMain() void {
    queue = ove.Queue(u32, 8).create() catch return;
    worker_th = ove.Thread(4096).create("worker", worker,
                                        ove.thread.prio.normal) catch return;
    ove.run();
    /* if (worker_th) |t| t.deinit();
     * if (queue)     |q| q.deinit();  -- on shutdown */
}

comptime { ove.exportMain(appMain); }
```

**Zero-heap** — file-scope wrappers embed the kernel-object storage and
(for `Thread`/`Workqueue`) the thread stack inline.  `init()` fills in
`&self.storage` / `&self.stack` and registers the kernel object against
those addresses; no global allocator is linked:

```zig
const ove = @import("ove");

fn worker() void { /* ... */ }

var queue: ove.Queue(u32, 8) = undefined;
var worker_th: ove.Thread(4096) = undefined;

fn appMain() void {
    queue.init() catch return;
    worker_th.init("worker", worker, ove.thread.prio.normal) catch return;
    ove.run();
}

comptime { ove.exportMain(appMain); }
```

### C (the binding substrate)

The C surface is what every higher-level binding lowers to. Use it directly when you want the smallest possible footprint, or when you're linking oveRTOS into existing C firmware.

**Heap** (`CONFIG_OVE_ZERO_HEAP` not set):

```c
#include "ove/ove.h"

static void worker_fn(void *arg) { /* ... */ }

void ove_main(void)
{
    ove_queue_t  queue;
    ove_mutex_t  mutex;
    ove_thread_t worker;

    ove_queue_create(&queue, sizeof(uint32_t), 8);
    ove_mutex_create(&mutex);
    ove_thread_create(&worker, "worker", worker_fn, NULL,
                      OVE_PRIO_NORMAL, 4096);

    ove_run();
    /* ove_thread_destroy(worker); ove_mutex_destroy(mutex);
     * ove_queue_destroy(queue);  -- on shutdown */
}
```

**Zero-heap** (`CONFIG_OVE_ZERO_HEAP=y`) — file-scope statics, allocator
never linked:

```c
#include "ove/ove.h"

static void worker_fn(void *arg) { /* ... */ }

OVE_QUEUE_DEFINE_STATIC(queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(mutex);
OVE_THREAD_DEFINE_STATIC(worker, 4096, worker_fn, NULL,
                         OVE_PRIO_NORMAL, "worker");

void ove_main(void)
{
    /* DEFINE_STATIC macros emit __attribute__((constructor)) hooks
     * that call ove_*_init() with caller-owned BSS storage before
     * ove_main() runs.  No _create() symbols in this build. */
    ove_run();
}
```

The two modes are wired through the same FFI:
`ove_*_create`/`ove_*_destroy` enter the path through the system heap
(gated by the per-module `OVE_HEAP_*` Kconfig); `ove_*_init`/
`ove_*_deinit` always operate against caller-owned storage.  In
zero-heap builds the `_create` symbols are not linked, so any accidental
heap-mode call site fails at link time rather than at runtime.  See
[Heap and Zero-Heap Modes](#heap-and-zero-heap-modes) below for the
allocation-discipline contract and
[docs-site/docs/examples](docs-site/docs/examples) for full apps in
every binding.

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

## Heap and Zero-Heap Modes

For memory-constrained or safety-critical systems, oveRTOS supports fully static allocation. The C API is split between heap-allocating and static-allocating entry points:

| API | Heap mode | Zero-heap mode |
|-----|-----------|----------------|
| `ove_*_init(handle, storage, ...)` / `ove_*_deinit(handle)` | available | available |
| `ove_*_create(handle, ...)` / `ove_*_destroy(handle)` | available (heap-allocated) | **link error** (gated by `OVE_HEAP_*`) |
| `OVE_*_DEFINE_STATIC(...)` | available (true static) | available (true static) |

Heap mode (`CONFIG_OVE_ZERO_HEAP` not set):

```c
ove_queue_t q;
ove_mutex_t m;

ove_queue_create(&q, sizeof(uint32_t), 8);
ove_mutex_create(&m);
/* ... */
ove_mutex_destroy(m);
ove_queue_destroy(q);
```

Zero-heap mode (`CONFIG_OVE_ZERO_HEAP=y`) -- use `OVE_*_DEFINE_STATIC` for file-scope objects, or `_init` with caller-supplied storage:

```c
OVE_QUEUE_DEFINE_STATIC(my_queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(my_mutex);
OVE_THREAD_DEFINE_STATIC(my_thread, 4096, worker_fn, NULL, OVE_PRIO_NORMAL, "worker");
```

The `OVE_*_DEFINE_STATIC` helpers also work in heap mode -- the macros expand to a true static allocation in either configuration, so they coexist with `ove_*_create()` for code that mixes both styles.

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
make test-renode-stm32f746-freertos          # Real STM32F7 HAL under Renode (heap)
make test-renode-stm32f746-freertos-zeroheap # Real STM32F7 HAL under Renode (zero-heap)
```

The Renode targets run the full CMocka suite against the same STM32F7
HAL + FreeRTOS ARM_CM7 port that ships on real hardware — the same
`firmware.elf` would boot on a Discovery board.  Renode is downloaded
automatically on first run into `output/tools/renode/`.  See
[tests/MATRIX.md](tests/MATRIX.md) for the full matrix and caveats
(IWDG / SAI / FMC aren't modelled).

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
