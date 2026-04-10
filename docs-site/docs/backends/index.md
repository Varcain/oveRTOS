# Backends

oveRTOS uses a backend to map its portable API onto a specific RTOS or threading environment. Each backend lives under `backends/<name>/` and provides one implementation file per subsystem (e.g. `freertos_thread.c`, `posix_sync.c`).

A `backends/common/` directory contains utilities shared across all backends (`ove_backend_common.h`).

## Available backends

| Backend | Directory | RTOS | Primary use |
|---------|-----------|------|-------------|
| FreeRTOS | `backends/freertos/` | FreeRTOS | Embedded hardware targets (Cortex-M) |
| POSIX | `backends/posix/` | pthreads | Host-PC desktop development and CI |
| Zephyr | `backends/zephyr/` | Zephyr RTOS | Zephyr-based embedded projects |
| NuttX | `backends/nuttx/` | NuttX | NuttX-based embedded projects |
| WASM | `backends/wasm/` | Emscripten | WebAssembly in-browser targets |

## Feature support matrix

Each cell indicates whether the subsystem is implemented by that backend. All backends implement the full core and communication APIs.

| Subsystem | FreeRTOS | POSIX | Zephyr | NuttX | WASM |
|-----------|----------|-------|--------|-------|------|
| Thread | Yes | Yes | Yes | Yes | Yes |
| Sync (mutex, semaphore, event, condvar) | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| Queue | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| Timer | Yes | Yes | Yes | Yes | Yes |
| EventGroup | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| WorkQueue | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| Stream | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| Time (monotonic clock, delays) | Yes | Yes | Yes | Yes | Yes |
| Console (UART) | Yes | Yes | Yes | Yes | Yes (browser console) |
| Board init | Yes | Yes | Yes | Yes | Yes (via sim) |
| GPIO | Yes | Yes (simulated) | Yes | Yes | Stub |
| LED | Yes | Yes (simulated) | Yes | Yes | Stub |
| Audio | Yes | Yes (simulated) | Yes | Yes | Yes (via sim) |
| FS (VFS) | Yes | Yes | Yes | Yes | No |
| NVS | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| Watchdog | Yes | Yes (simulated) | Yes | Yes | Yes (simulated) |
| Shell | Yes | Yes | Yes | Yes | Yes (via POSIX) |
| PM (power management) | Yes | Yes (simulated) | Yes | Yes | Yes (via sim) |
| LVGL display | Yes | Yes (sim dashboard) | Yes | Yes | Yes (HTML canvas) |

## Backend file layout

Each backend follows the same naming convention: `<backend>_<subsystem>.c`. For example:

```
backends/freertos/
    freertos_thread.c
    freertos_sync.c
    freertos_queue.c
    freertos_timer.c
    freertos_eventgroup.c
    freertos_workqueue.c
    freertos_stream.c
    freertos_time.c
    freertos_console.c
    freertos_board.c
    freertos_gpio.c
    freertos_led.c        (via freertos_board.c)
    freertos_audio.c
    freertos_fs.c
    freertos_nvs.c
    freertos_watchdog.c
    freertos_shell.c
    freertos_lvgl.c
    freertos_heap_stubs.c
    include/              (backend-private headers)
```

The POSIX, Zephyr, and NuttX backends follow the same layout with their respective prefix.

## Selecting a backend

The backend is chosen by the board's CMakeLists. It defines `CONFIG_OVE_RTOS_<NAME>` and adds the relevant backend source files to `OVE_BACKEND_SOURCES`. Application code never needs to reference a backend directly — all API calls go through `ove/ove.h`.

## FreeRTOS backend notes

- Uses FreeRTOS tasks for threads, with the oveRTOS priority enum mapped linearly onto FreeRTOS task priorities.
- Heap mode uses `pvPortMalloc` / `vPortFree`; zero-heap mode uses static FreeRTOS object APIs.
- `freertos_heap_stubs.c` provides `malloc`/`free` shims for third-party libraries (e.g. LVGL) that call the C allocator directly.
- Timer callbacks run in the FreeRTOS timer service task context.

## POSIX backend notes

- Maps oveRTOS threads onto `pthread_t` with a scheduling policy derived from the priority level.
- Binary events use a `pthread_cond_t` + `pthread_mutex_t` pair.
- Time functions use `clock_gettime(CLOCK_MONOTONIC)`.
- GPIO and LED are simulated in memory; audio is routed through the sim framework to the browser dashboard.
- The scheduler "start" call on POSIX simply waits for all spawned threads to finish, which allows the application to return cleanly from `main`.

## Zephyr backend notes

- Threads map to Zephyr kernel threads; the priority enum is translated to Zephyr cooperative/preemptive priorities.
- Synchronization primitives use `k_mutex`, `k_sem`, `k_condvar`, and `k_poll_signal`.
- Timers use `k_timer`.

## NuttX backend notes

- Threads map to NuttX `pthread` or `task_create` depending on configuration.
- Filesystem operations delegate to the NuttX VFS layer.

## WASM backend notes

- Compiles to WebAssembly via Emscripten and runs in a web browser.
- Threads use Emscripten's `pthread` emulation with `SharedArrayBuffer`.
- Display renders via the sim dashboard into an HTML5 `<canvas>` element.
- Audio uses the browser's Web Audio API through the sim framework.
- Bus drivers (UART, SPI, I2C) are stubbed — hardware peripherals are not available in the browser.
- HTTP and SNTP use Emscripten's native fetch/XHR support.
- Networking sockets are stubbed (no raw TCP/UDP in browsers).
