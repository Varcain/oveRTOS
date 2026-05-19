# oveRTOS

[![tests](https://github.com/Varcain/oveRTOS/actions/workflows/ove-tests.yml/badge.svg)](https://github.com/Varcain/oveRTOS/actions/workflows/ove-tests.yml)
[![coverage](https://img.shields.io/endpoint?url=https://gist.githubusercontent.com/Varcain/2311c373541b067eeb3db3fa9580340b/raw/overtos-coverage.json)](https://github.com/Varcain/oveRTOS/actions/workflows/coverage.yml)

Write embedded RTOS applications in **C++**, **Rust**, or **Zig** — and run them unchanged on **FreeRTOS**, **Zephyr**, or **Apache NuttX**.

First-class language bindings sit on top of a unified C application API: threads, synchronisation, queues, timers, networking (sockets / TLS / HTTP / MQTT / SNTP / HTTPD), audio graph, ML inference, LVGL, power management, NVS, filesystem, shell, watchdog, bus drivers. The binding picks the ergonomics; the kernel underneath is a configure-time choice with [minimal runtime overhead](https://varcain.github.io/oveRTOS/benchmarks/). Native POSIX and WebAssembly backends are available for development and browser-hosted demos.

> **New here?** → [**Quickstart in 5 minutes**](https://varcain.github.io/oveRTOS/getting-started/quickstart/). Hit a wall? → run `make doctor` or browse the [Troubleshooting page](https://varcain.github.io/oveRTOS/getting-started/troubleshooting/). Building an app outside the tree? → [`ove app new`](https://varcain.github.io/oveRTOS/build-system/external-apps/) stamps a working skeleton.

## Key features

- **Modern-language bindings** — C++23 with `std::expected`-based `Result<T>`, Rust `no_std` + `alloc`, Zig with `comptime`-generic wrappers and allocator-aware constructors. Plus the underlying C surface.
- **Cross-RTOS portability** — same source on FreeRTOS, Zephyr, and NuttX; backend chosen at configure time via Kconfig (`CONFIG_OVE_RTOS_*`).
- **Application-grade modules** — sockets / TLS / HTTP / MQTT / HTTPD / SNTP, audio graph, TFLite Micro inference, LVGL widgets, NVS, filesystem, power management, shell, watchdog, UART / SPI / I2C / I2S.
- **Two allocation modes** — heap (`_create()` / `_destroy()`) or zero-heap (`_init()` / `_deinit()` with caller storage, or `OVE_*_DEFINE_STATIC` for file-scope objects). The `_DEFINE_STATIC` macros compile identically in both modes.
- **[Minimal abstraction overhead](https://varcain.github.io/oveRTOS/benchmarks/)** — compile-time backend dispatch, no vtables, no indirect calls. Per-op wrapper cost is benchmarked across every binding on every commit.
- **POSIX + WebAssembly dev backends** — develop on Linux/macOS without hardware; demo the same firmware in a browser.

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

## Quick start

Prereqs: Python 3 + `venv`, CMake, a host C/C++ compiler. ARM-GCC and QEMU are downloaded automatically when an embedded target is selected.

```bash
make host.posix.example_cpp   # load <board>.<rtos>.<app> config
make                          # download + configure + build
make run                      # launch
```

Swap the third token for another language: `example_c`, `example_rust`, `example_zig`. Append `_zh` for the zero-heap variant. The dot-syntax extends to any `<board>.<rtos>.<app>` combination — `make help` enumerates everything from the live tree, or run `make menuconfig` for an interactive picker. See [Getting Started → Configure](https://varcain.github.io/oveRTOS/getting-started/configure/) for the full grammar.

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

## API overview

The four snippets below are the **same** runnable producer / consumer / shared-state pattern in each binding, in both heap and zero-heap modes. A producer thread emits an incrementing counter into a bounded queue; a consumer thread receives it, takes a mutex to update a piece of shared state, and logs every fifth value. This is the shape of every starter app under [`apps/{c,cpp,rust,zig}/heap/example/`](https://github.com/Varcain/oveRTOS/tree/main/apps).

Every binding lowers to the same FFI symbols. Per-call wrapper overhead is benchmarked across the matrix at [varcain.github.io/oveRTOS/benchmarks](https://varcain.github.io/oveRTOS/benchmarks/).

### C++23 — RAII, `Result<T>`, std-composable

Built with `-std=c++23`. Fallible operations return `ove::Result<T>` (an alias for `std::expected<T, ove::Error>`); forever-blocking forms return `void` and abort on substrate failure. `ove::Mutex` satisfies the `Lockable` named requirement and composes directly with `std::lock_guard` / `std::scoped_lock`.

**Heap** — kernel objects allocated by `_create()` under the hood; the wrapper itself lives on the heap via `std::make_unique`. The `unique_ptr` destructors at shutdown invoke `ove_*_destroy` and free the wrapper.

```cpp
#include <ove/ove.hpp>
#include <memory>
using namespace std::chrono_literals;

/* Pointer holders are file-scope so the C-callback thread entries can
 * reach them; the underlying ove::* objects are heap-allocated inside
 * ove_main() via std::make_unique. */
static std::unique_ptr<ove::Queue<uint32_t, 8>> counter_queue;
static std::unique_ptr<ove::Mutex>              value_mutex;
static std::unique_ptr<ove::Thread<4096>>       prod_thread;
static std::unique_ptr<ove::Thread<4096>>       cons_thread;
static uint32_t                                 last_value;

static void producer_fn(void *)
{
    uint32_t n = 0;
    while (true) {
        ++n;
        if (!counter_queue->try_send_for(n, 1s)) {     // Result<void>
            OVE_LOG_WRN("producer: dropped %u", n);
        }
        ove::this_thread::sleep_for(500ms);
    }
}

static void consumer_fn(void *)
{
    uint32_t v;
    while (true) {
        counter_queue->receive(v);                     // forever-blocking, void
        {
            std::lock_guard<ove::Mutex> g(*value_mutex);
            last_value = v;
        }
        if (v % 5 == 0) {
            OVE_LOG_INF("consumer: count = %u", v);
        }
    }
}

void ove_main()
{
    counter_queue = std::make_unique<ove::Queue<uint32_t, 8>>();
    value_mutex   = std::make_unique<ove::Mutex>();
    prod_thread   = std::make_unique<ove::Thread<4096>>(
        producer_fn, nullptr, OVE_PRIO_NORMAL, "producer");
    cons_thread   = std::make_unique<ove::Thread<4096>>(
        consumer_fn, nullptr, OVE_PRIO_NORMAL, "consumer");

    ove::run();
}
```

**Zero-heap** — same wrappers, file-scope objects: the wrapper class carries the kernel-object storage (and thread stack) inline as members; the constructors run during static init and call `ove_*_init()` with pointers into those members. No heap, no `operator new`. Move/copy are deleted on every wrapper in zero-heap mode so each instance is structurally pinned to its file-scope address.

```cpp
#include <ove/ove.hpp>

static ove::Queue<uint32_t, 8> counter_queue;
static ove::Mutex              value_mutex;
static uint32_t                last_value;

static void producer_fn(void *) { /* uses counter_queue directly */ }
static void consumer_fn(void *) { /* uses counter_queue + value_mutex */ }

static ove::Thread<4096> prod_thread(producer_fn, nullptr,
                                     OVE_PRIO_NORMAL, "producer");
static ove::Thread<4096> cons_thread(consumer_fn, nullptr,
                                     OVE_PRIO_NORMAL, "consumer");

void ove_main() { ove::run(); }
```

**Other std-mirror conveniences:** `ove::stop_token` / `ove::stop_source` for cooperative cancellation, `ove::Error` round-tripping through `std::error_code`, and `Result<T>` returns on every payload-bearing call (`http_client.get(...)` → `Result<Response>`, `ove::dns::resolve(...)` → `Result<Address>`, `ove::sntp::get_utc()` → `Result<uint32_t>`).

### Rust — `no_std`, errors-as-values

Targets stable Rust with `#![cfg_attr(not(feature = "std"), no_std)]`. Fallible operations return `Result<T, ove::Error>` with **per-op narrow error sets** (`try_send_for` can only fail with `Timeout` / `QueueFull`); forever-blocking forms return `T` directly. `#[ove::main]` exports the entry; the standard `log` crate routes through the oveRTOS console after `ove::log::try_init()`.

**Heap** — `Type::new()` constructors return `Result<Self>`; `Arc<T>` / `Box<T>` / `Vec<T>` re-exported from `ove::heap`. Threads spawn via `Thread::builder()` taking a closure that receives a cooperative `StopToken`:

```rust
#![cfg_attr(not(feature = "std"), no_std)]
use core::time::Duration;
use ove::heap::Arc;
use ove::sync::Mutex;
use ove::{Priority, Queue, Thread};

#[ove::main]
fn app_main() {
    ove::log::try_init();

    let queue = Arc::new(Queue::<u32, 8>::new().expect("queue"));
    let state = Arc::new(Mutex::new(0u32).expect("mutex"));   // data-carrying Mutex<T>

    let q = Arc::clone(&queue);
    let _producer = Thread::builder()
        .name(c"producer")
        .priority(Priority::Normal)
        .stack_size(4096)
        .spawn(move |stop| {
            let mut n: u32 = 0;
            while !stop.is_stopped() {
                n += 1;
                match q.try_send_for(&n, Duration::from_millis(1000)) {
                    Ok(()) => {}
                    Err(ove::Error::Timeout)   => log::warn!("send timeout"),
                    Err(ove::Error::QueueFull) => log::warn!("dropped {n}"),
                    Err(_) => unreachable!(),
                }
                Thread::sleep_ms(500);
            }
        })
        .expect("spawn");

    let q = Arc::clone(&queue);
    let s = Arc::clone(&state);
    let _consumer = Thread::builder()
        .name(c"consumer")
        .stack_size(4096)
        .spawn(move |_stop| loop {
            let v = q.recv().unwrap();        // forever recv() → T, infallible
            *s.lock().unwrap() = v;           // MutexGuard<u32>, Deref to T
            if v % 5 == 0 { log::info!("count = {v}"); }
        })
        .expect("spawn");

    ove::run();
}
```

**Zero-heap** — `Type::from_static(&mut storage, ...)` against caller-owned BSS; the `ove::shared!`, `ove::mutex!`, `ove::queue!`, `ove::thread!` macros collapse the storage-decl + constructor boilerplate while preserving the static-origin guarantee. No `Box`, `Arc`, or `alloc`:

```rust
#![cfg_attr(not(feature = "std"), no_std)]
use core::time::Duration;
use ove::{Priority, Queue, Thread};

ove::shared!(QUEUE: Queue<u32, 8>);
ove::shared!(LAST:  ove::sync::Mutex<u32>);

#[ove::main]
fn app_main() {
    ove::log::try_init();
    QUEUE.init(ove::queue!(u32, 8));
    LAST.init(ove::mutex!(0u32));

    let _producer = Thread::builder()
        .name(c"producer").priority(Priority::Normal).stack_size(4096)
        .spawn(|_stop| {
            let mut n: u32 = 0;
            loop {
                n += 1;
                let _ = QUEUE.try_send_for(&n, Duration::from_millis(1000));
                Thread::sleep_ms(500);
            }
        })
        .expect("spawn");

    let _consumer = Thread::builder()
        .name(c"consumer").stack_size(4096)
        .spawn(|_stop| loop {
            let v = QUEUE.recv().unwrap();
            *LAST.lock().unwrap() = v;
            if v % 5 == 0 { log::info!("count = {v}"); }
        })
        .expect("spawn");

    ove::run();
}
```

**Highlights:** data-carrying `Mutex<T>` mirrors `std::sync::Mutex<T>`; per-op narrow error sets (exhaustive `match` without `_ =>` fallback); `Stream<N>` and `fs::File` implement `embedded_io::Read` / `Write`; `Thread::sleep_until(deadline)` and `q.try_recv_until(deadline)` share a single deadline across a sequence of bounded waits.

### Zig — comptime-safe wrappers, allocator-aware

Targets Zig 0.15+. Every wrapper takes a `std.mem.Allocator` — `std.heap.page_allocator` in heap mode, a `FixedBufferAllocator` over a BSS arena in zero-heap mode — and the wrapper itself works the same in both. Per-op error sets are **structurally narrow** at the type system (`error{ QueueFull, Timeout }`, `error{Timeout}`, …) so exhaustive `switch` arms catch every reachable case with no `else =>` fallback. `std.log.*` integrates via `ove.log.logFn`; `ove.target.current_rtos` is a typed enum so the compiler flags non-exhaustive switches.

**Heap:**

```zig
const std = @import("std");
const ove = @import("ove");

pub const std_options: std.Options = .{ .logFn = ove.log.logFn };

const app_allocator = std.heap.page_allocator;

var queue: ove.Queue(u32, 8) = undefined;
var last_value: u32 = 0;
var state_mutex: ove.Mutex = undefined;

fn producerEntry() void {
    var n: u32 = 0;
    while (true) {
        n += 1;
        queue.sendFor(&n, .millis(1000)) catch |e| switch (e) {
            error.Timeout   => std.log.warn("send timeout", .{}),
            error.QueueFull => std.log.warn("dropped {d}", .{n}),
        };
        ove.thread.sleepMs(500);
    }
}

fn consumerEntry() void {
    while (true) {
        const v = queue.recv();                  // forever-blocking, infallible
        state_mutex.lock();
        last_value = v;
        state_mutex.unlock();
        if (v % 5 == 0) std.log.info("count = {d}", .{v});
    }
}

fn appMain() void {
    queue       = ove.Queue(u32, 8).create(app_allocator) catch return;
    state_mutex = ove.Mutex.create(app_allocator) catch return;

    _ = ove.Thread(4096).spawn(app_allocator,
        .{ .name = "producer", .priority = .normal }, producerEntry, .{}) catch return;
    _ = ove.Thread(4096).spawn(app_allocator,
        .{ .name = "consumer", .priority = .normal }, consumerEntry, .{}) catch return;

    ove.run();
}

comptime { ove.exportMain(appMain); }
```

**Zero-heap** — same wrapper, same `create(allocator)` call. Only the allocator changes: a `FixedBufferAllocator` over a static BSS arena routes every byte to caller-owned memory. `ove.allocators.c_allocator` / `page_allocator` / `GeneralPurposeAllocator` become `@compileError` under `CONFIG_OVE_ZERO_HEAP`, so an accidental dynamic-allocator import fails at build time:

```zig
const std = @import("std");
const ove = @import("ove");

pub const std_options: std.Options = .{ .logFn = ove.log.logFn };

var arena_bytes: [4096]u8 = undefined;
var fba: std.heap.FixedBufferAllocator = undefined;

var queue: ove.Queue(u32, 8) = undefined;
var state_mutex: ove.Mutex = undefined;
var producer_th: ove.Thread(4096) = undefined;
var consumer_th: ove.Thread(4096) = undefined;
var last_value: u32 = 0;

fn appMain() void {
    fba = std.heap.FixedBufferAllocator.init(&arena_bytes);
    const allocator = fba.allocator();

    queue       = ove.Queue(u32, 8).create(allocator) catch return;
    state_mutex = ove.Mutex.create(allocator) catch return;

    producer_th = ove.Thread(4096).spawn(allocator,
        .{ .name = "producer", .priority = .normal }, producerEntry, .{}) catch return;
    consumer_th = ove.Thread(4096).spawn(allocator,
        .{ .name = "consumer", .priority = .normal }, consumerEntry, .{}) catch return;

    ove.run();
}

comptime { ove.exportMain(appMain); }
```

**Highlights:** one API across both modes (`Type.create(allocator)`); typed `Duration` / `Instant` with saturating arithmetic and deadline-aware variants (`q.sendUntil(item, deadline)`, `cv.timedWaitUntil(...)`); `std.log.*` integration with a one-line `std_options` declaration; exhaustive `switch (ove.target.current_rtos)` enforced by the compiler; `Queue(T, N)` `@compileError`s if `T` declares a `deinit()` method (substrate `memcpy`s items).

### C — the binding substrate

The C surface is what every higher-level binding lowers to. Use it directly for the smallest footprint or when integrating with existing C firmware.

**Heap:**

```c
#include "ove/ove.h"

#define QUEUE_DEPTH 8

static ove_queue_t  counter_queue;
static ove_mutex_t  value_mutex;
static ove_thread_t producer_handle;
static ove_thread_t consumer_handle;
static uint32_t     last_value;

static void producer_thread(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    while (1) {
        ++count;
        int rc = ove_queue_send(counter_queue, &count, OVE_MS(1000));
        if (rc != OVE_OK) {
            OVE_LOG_WRN("producer: queue full, dropped %u", count);
        }
        ove_thread_sleep_ms(500);
    }
}

static void consumer_thread(void *arg)
{
    (void)arg;
    uint32_t val;
    while (1) {
        if (ove_queue_receive(counter_queue, &val, OVE_WAIT_FOREVER) == OVE_OK) {
            ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
            last_value = val;
            ove_mutex_unlock(value_mutex);

            if (val % 5 == 0) {
                OVE_LOG_INF("consumer: count = %u", val);
            }
        }
    }
}

void ove_main(void)
{
    ove_queue_create(&counter_queue, sizeof(uint32_t), QUEUE_DEPTH);
    ove_mutex_create(&value_mutex);
    ove_thread_create(&producer_handle, "producer", producer_thread, NULL,
                      OVE_PRIO_NORMAL, 4096);
    ove_thread_create(&consumer_handle, "consumer", consumer_thread, NULL,
                      OVE_PRIO_NORMAL, 4096);
    ove_run();
}
```

**Zero-heap** — file-scope statics, allocator never linked. The `OVE_*_DEFINE_STATIC` macros emit `__attribute__((constructor))` hooks that call `ove_*_init()` with caller-owned BSS storage before `ove_main()` runs:

```c
#include "ove/ove.h"

#define QUEUE_DEPTH 8

OVE_QUEUE_DEFINE_STATIC(counter_queue, sizeof(uint32_t), QUEUE_DEPTH);
OVE_MUTEX_DEFINE_STATIC(value_mutex);
OVE_THREAD_DEFINE_STATIC(producer_handle, 4096, producer_thread, NULL,
                         OVE_PRIO_NORMAL, "producer");
OVE_THREAD_DEFINE_STATIC(consumer_handle, 4096, consumer_thread, NULL,
                         OVE_PRIO_NORMAL, "consumer");
static uint32_t last_value;

/* producer_thread / consumer_thread defined as in the heap example */

void ove_main(void) { ove_run(); }
```

The two modes share the same FFI. `ove_*_create` / `_destroy` go through the RTOS heap (gated by per-module `OVE_HEAP_*` Kconfig); `ove_*_init` / `_deinit` always operate against caller-owned storage. In zero-heap builds the `_create` symbols are not linked, so any accidental heap-mode call site fails at link time. Full apps in every binding: [`apps/{c,cpp,rust,zig}/{heap,zeroheap}/`](https://github.com/Varcain/oveRTOS/tree/main/apps).

## Modules

| Module | Description |
|--------|-------------|
| `ove_thread` | Thread lifecycle, priority, sleep, yield, cooperative stop |
| `ove_sync` | Mutex, recursive mutex, counting semaphore, binary event, condition variable |
| `ove_queue` | Fixed-size FIFO message queues |
| `ove_timer` | Software timers (periodic / one-shot) |
| `ove_time` | Monotonic clock, delays |
| `ove_eventgroup` | Multi-bit event flags |
| `ove_workqueue` | Deferred work execution |
| `ove_stream` | Byte-stream ring buffers |
| `ove_console` | UART serial I/O |
| `ove_uart` | UART driver — configurable baud / framing, async RX |
| `ove_spi` | SPI master — software CS, thread-safe bus locking |
| `ove_i2c` | I2C master — register-level convenience APIs |
| `ove_i2s` | I2S audio bus — DMA double-buffered streaming |
| `ove_gpio` | Digital I/O, edge-triggered ISRs |
| `ove_led` | LED control |
| `ove_audio` | Graph-based audio engine with typed nodes |
| `ove_net` | TCP/UDP sockets, DNS, TLS, HTTP, MQTT, HTTPD, SNTP |
| `ove_infer` | ML inference (LiteRT / TensorFlow Lite Micro), optional CMSIS-NN |
| `ove_fs` | Virtual filesystem |
| `ove_nvs` | Non-volatile key-value storage |
| `ove_lvgl` | LVGL 9.x display integration with thread-safe lock |
| `ove_shell` | Interactive command shell |
| `ove_log` | Compile-time filtered logging |
| `ove_watchdog` | Hardware watchdog |
| `ove_pm` | Power management — sleep states, domains, wake sources, policy |

## Debugging

Code-level debugging is available on both the QEMU and WASM targets:

- **QEMU**: the browser dashboard includes a Debug window (Monaco source view, breakpoints, step/pause/continue, call stack, registers) driven by `arm-none-eabi-gdb` attached to QEMU's GDB stub.
- **WASM**: build with `-DOVE_DEBUG=ON` to embed DWARF, install the [C/C++ DevTools Support (DWARF)](https://chromewebstore.google.com/detail/cc-devtools-support-dwarf/pdcpmagijalfljmkmjngeonclgbbannb) Chrome extension, then F12 → Sources panel and set breakpoints in your `.c` files. Optional `-DOVE_WASM_SAFE=ON` adds `SAFE_HEAP` / `ASSERTIONS` / stack-overflow-check for memory-bug hunting.

Full workflow: [Debugging](docs-site/docs/getting-started/run.md#debugging).

## Testing

```bash
make test              # Simulator tests (stub, C++, Rust, Zig, NuttX, Zephyr)
make test-qemu         # All QEMU ARM tests
make test-all          # Everything (sim + QEMU + Renode; HW excluded)
```

Individual targets:

```bash
make test-stub                              # Stub backend API tests (POSIX, ~2 s)
make test-cpp                               # C++ binding tests
make test-rust                              # Rust binding tests
make test-zig                               # Zig binding tests
make test-nuttx                             # NuttX simulator tests
make test-zephyr                            # Zephyr native_sim tests
make test-qemu-<rtos>{,-zeroheap}           # QEMU MPS2-AN500 (rtos ∈ freertos/nuttx/zephyr)
make test-renode-stm32f746-<rtos>{,-zeroheap}  # Renode STM32F7 silicon model
make test-hw-stm32f746-<rtos>{,-zeroheap}   # Real Discovery board over OpenOCD (manual)
```

The Renode targets run the full CMocka suite against the same STM32F7 HAL + RTOS port that ships on real hardware — the same `firmware.elf` would boot on a Discovery board. Renode is downloaded automatically on first run into `output/tools/renode/`. See [tests/MATRIX.md](tests/MATRIX.md) for the full matrix and modelling caveats (IWDG / SAI / FMC).

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

The `backend-struct` rule enforces a key invariant: every `ove_*_storage_t` is an alias for the same `struct ove_X` declared in `backends/<rtos>/include/ove_storage_<rtos>.h`. Backend `.c` files must **never** declare `struct ove_X` at top level (narrow FS allowlist aside) — otherwise consumers in other translation units see a different size than the backend writes and `_init()` silently corrupts adjacent memory. See `config/ove-cli/ove/lint_backend_struct.py` for the allowlist.

Before pushing, run `make lint && make test`. CI's first job (`.github/workflows/ove-tests.yml::lint`) gates every downstream test job, so a lint failure blocks the whole pipeline fast.

## Documentation

```bash
make docs          # Build complete documentation site (C, C++, Rust, Zig API + guides)
make docs-serve    # Build and serve locally at http://localhost:8000
```

## Project structure

```
oveRTOS/
├── include/ove/        # Public C API headers
├── src/                # Core framework implementation
├── backends/           # Backend implementations
│   ├── freertos/  nuttx/  zephyr/  posix/  wasm/  common/
├── bindings/           # Language bindings
│   ├── cpp/  rust/  zig/
├── apps/               # Example applications
│   ├── c/{heap,zeroheap}/        # example, example_net, example_pm,
│   ├── cpp/{heap,zeroheap}/      # example_keyword_live, lvgl_benchmark
│   ├── rust/{heap,zeroheap}/     # (+ lvgl_gallery in cpp/rust/zig)
│   └── zig/{heap,zeroheap}/
├── models/             # ML model assets (TFLite)
├── sim/                # Simulation framework (plugins, dashboard, transports)
├── boards/             # Board definitions
├── config/             # Kconfig definitions and the `ove` CLI
│   └── fragments/      # Config fragments (board, RTOS, app)
├── tests/              # Test suites + benchmark harnesses
└── docs-site/          # MkDocs documentation site
```

## License

Copyright (C) 2026 Kamil Lulko

This project is licensed under the [GNU General Public License v3.0 or later](LICENSE).

See [NOTICE](NOTICE) for third-party attribution.
