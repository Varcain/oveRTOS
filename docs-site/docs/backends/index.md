# Backends

!!! note "Audience"
    This page is for **oveRTOS developers** — porting to a new RTOS, hacking on existing backends, or sizing the kernel layer. App developers can skip to [API Reference](../api/index.md).

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
| Thread | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| Sync (mutex, semaphore, event, condvar) | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| Queue | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| Timer | Yes | Yes | Yes | Yes | Yes |
| EventGroup | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| WorkQueue | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| Stream | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| Time (monotonic clock, delays) | Yes | Yes | Yes | Yes | Yes |
| Console (UART) | Yes | Yes | Yes | Yes | Yes (browser console) |
| Board init | Yes | Yes | Yes | Yes | Yes (via sim) |
| GPIO | Yes | Yes (simulated) | Yes | Yes | Stub |
| LED | Yes | Yes (simulated) | Yes | Yes | Stub |
| Audio | Yes | Yes (simulated) | Yes | Yes | Yes (via sim) |
| FS (VFS) | Yes | Yes | Yes | Yes | No |
| NVS | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
| Watchdog | Yes | Yes (simulated) | Yes | Yes | Yes (simulated) |
| Shell | Yes | Yes | Yes | Yes | Yes (emscripten pthreads) |
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

## Peripheral wrappers

What each `ove_*` peripheral wrapper resolves to per backend. These are useful when porting boards or debugging the lowest layer; app code never references these directly.

### UART (`ove/uart.h`)

| Backend | Implementation |
|---|---|
| FreeRTOS/STM32 | `HAL_UART_Init()`, RXNE interrupt, `HAL_UART_Transmit()` |
| Zephyr | `uart_configure()`, IRQ callback via `uart_irq_rx_ready()` |
| NuttX | `/dev/ttyS*`, termios, RX polling thread |
| POSIX | `/dev/ttyUSB*` or `/dev/ttyACM*`, termios, RX polling thread |

### SPI (`ove/spi.h`)

| Backend | Implementation |
|---|---|
| FreeRTOS/STM32 | `HAL_SPI_TransmitReceive()` / `Transmit()` / `Receive()` |
| Zephyr | `spi_transceive()` with `spi_buf_set` |
| NuttX | `/dev/spi*`, `SPIIOC_TRANSFER` ioctl |
| POSIX | `/dev/spidev*`, `SPI_IOC_MESSAGE` ioctl |

### I²C (`ove/i2c.h`)

| Backend | Implementation |
|---|---|
| FreeRTOS/STM32 | `HAL_I2C_Master_Transmit/Receive()`, `HAL_I2C_Mem_Read()` |
| Zephyr | `i2c_write()`, `i2c_read()`, `i2c_write_read()` |
| NuttX | `/dev/i2c*`, `I2CIOC_TRANSFER` ioctl |
| POSIX | `/dev/i2c-*`, `I2C_RDWR` ioctl |

## Power management

Per-backend behaviour of `ove_pm_*`. The portable surface lives at [API → Power Management](../api/pm.md); this section covers what the wrapper resolves to per backend and how to plug in a non-default policy.

| Backend | Idle entry | Wake sources | Default policy hook |
|---|---|---|---|
| FreeRTOS / STM32 | `__WFI()` from the idle hook | EXTI, RTC alarm, UART, USART RXNE | `freertos_pm.c` |
| Zephyr | `pm_state_set()` integration | Subsystem-defined | `zephyr_pm.c` |
| NuttX | `up_idle()` hook | Per-driver wake sources | `nuttx_pm.c` |
| POSIX | `sleep()` simulation | Sim plugin events | `posix_pm.c` |
| WASM | Sim-tick step | Browser events | `wasm_pm.c` |

### Power management policy

The default threshold-based policy transitions to deeper sleep states as idle duration increases. Replace it with `ove_pm_set_policy()`:

```c
ove_pm_state_t my_policy(ove_pm_state_t current, uint32_t idle_ms,
                         uint32_t next_timeout_ms, void *user_data)
{
    if (idle_ms > 5000 && next_timeout_ms > 10000)
        return OVE_PM_STATE_DEEP_SLEEP;
    return current;
}

ove_pm_set_policy(my_policy, NULL);
```

Pass `NULL` to restore the default policy. Pre-sleep / post-wake notifications are registered with `ove_pm_notify_register()`:

```c
void my_notify(ove_pm_event_t event, ove_pm_state_t from,
               ove_pm_state_t to, void *user_data)
{
    if (event == OVE_PM_EVENT_PRE_SLEEP)
        /* save state before sleep */;
    else if (event == OVE_PM_EVENT_POST_WAKE)
        /* restore state after wake */;
}

ove_pm_notify_register(my_notify, NULL);
```

## Networking backends

| Backend | Stack | Pool sizing |
|---|---|---|
| FreeRTOS | lwIP | [Backend tuning → Networking pools](net-tuning.md) |
| Zephyr | Native Zephyr net stack | [Backend tuning → Networking pools](net-tuning.md) |
| NuttX | Native NuttX networking | [Backend tuning → Networking pools](net-tuning.md) |
| POSIX | Host BSD sockets | Host kernel responsibility |
| WASM | Stubbed (HTTP/SNTP via emscripten fetch) | n/a |

## FreeRTOS backend notes

- Uses FreeRTOS tasks for threads, with the oveRTOS priority enum mapped linearly onto FreeRTOS task priorities.
- Heap mode uses `pvPortMalloc` / `vPortFree`; zero-heap mode uses static FreeRTOS object APIs.
- `freertos_heap_stubs.c` provides `malloc`/`free` shims for third-party libraries (e.g. LVGL) that call the C allocator directly.
- Timer callbacks run in the FreeRTOS timer service task context.
- **CI-runnable STM32 fidelity**: the `make test-renode-stm32f746-…` targets build the same HAL + RTOS port that ships on real Discovery hardware and run the full CMocka suite under Renode's `stm32f7_discovery-bb` emulation on every PR — covers FreeRTOS / Zephyr / NuttX × heap / zero-heap. The complementary `make test-hw-stm32f746-…` matrix flashes that same firmware onto an actual STM32F746G-Discovery via OpenOCD and reads back USART1 (manual-only — set `OVE_HW_SERIAL_PORT=/dev/ttyACMx`). See [Testing → Renode](../testing/renode.md) and [Testing → Hardware-in-the-loop](../testing/hw.md).
- **`ove_main()` locals are UB after `ove_run()`** (technical detail): `vTaskStartScheduler()` resets MSP to the initial stack top (see `prvPortStartFirstTask` in the Cortex-M port) and the first interrupt overwrites the region that held those locals. App developers see this rule in [Troubleshooting → ove_main locals](../getting-started/troubleshooting.md#ove_main-locals-are-ub-after-ove_run); this note is the why for the porting reader.

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
