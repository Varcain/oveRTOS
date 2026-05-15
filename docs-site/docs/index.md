# oveRTOS Documentation

oveRTOS is an embedded RTOS framework that provides a unified build system, Kconfig-based configuration, and portable API across **FreeRTOS**, **Apache NuttX**, **Zephyr RTOS**, and **POSIX** (with a browser-hosted **WASM** target). The build system downloads RTOS sources and cross-compilation toolchains, generates RTOS-native configuration files from a single `.config`, and orchestrates compilation through each backend's native build system. Applications written against the oveRTOS API compile unchanged for any supported backend: the correct implementation is selected at compile time via Kconfig preprocessor symbols, with no virtual dispatch tables and [minimal runtime overhead](benchmarks/index.md) over the native API. The core API is written in C, with first-class bindings for C++, Rust, and Zig. LVGL is integrated as the GUI toolkit, with backend-portable display and input driver support across all targets.

!!! tip "First time here?"
    Run through the [**Quickstart**](getting-started/quickstart.md) — three commands take you from a fresh clone to a running app in five minutes. No hardware required.

## Where to next

<div class="grid cards" markdown>

-   :material-rocket-launch: **Quickstart**

    ---

    `git clone` → `make setup` → `make doctor` → `make host.posix.example_c` → `make run`. The fastest path to a running oveRTOS app on your machine.

    [Start here →](getting-started/quickstart.md)

-   :material-folder-multiple: **Examples**

    ---

    Five reference apps across C, C++, Rust, and Zig, in both heap and zero-heap modes. Producer-consumer, networking, power management, on-device ML, LVGL rendering.

    [Browse examples →](examples/index.md)

-   :material-book-open-variant: **API Reference**

    ---

    Every public module with usage patterns, semantics, and gotchas: threads, sync, queues, timers, audio engine, networking, ML inference, filesystem, NVS, power management, hardware buses.

    [Open the reference →](api/index.md)

-   :material-package-variant: **Create your own app**

    ---

    Stamp a working external-app skeleton with one command, or follow the manual walkthrough. Live outside the oveRTOS tree, with your own Makefile and `app.yaml`.

    [Scaffold an app →](build-system/external-apps.md)

-   :material-chef-hat: **Cookbook**

    ---

    Pattern recipes that combine modules: read a sensor on a timer, persist a value across reboots, POST to HTTPS, run inference on audio, draw to LVGL with thread safety, add a shell command.

    [Open the cookbook →](cookbook/index.md)

-   :material-swap-horizontal: **Migrate existing code**

    ---

    Side-by-side API mappings if you're coming from raw FreeRTOS, Zephyr, or bare-metal C. What translates one-to-one, what doesn't, and where the rough edges live.

    [Compare APIs →](migration/from-freertos.md)

</div>

## Key features

- **Integrated build system** — downloads RTOS sources and toolchains, generates RTOS-native configuration, orchestrates cross-compilation via each backend's native build tools
- **35+ API modules** — threads, sync, queues, timers, event groups, work queues, streams, audio, filesystem, GPIO, LEDs, console, logging, shell, NVS, watchdog, networking (sockets, TLS, HTTP, MQTT, HTTPD, SNTP), power management, bus drivers (UART, SPI, I2C, I2S), ML inference, LVGL
- **4 RTOS backends** — FreeRTOS (via STM32CubeF7), Apache NuttX, Zephyr, POSIX with browser-based sim dashboard for native host development
- **4 language bindings** — C, C++ (RAII), Rust (`no_std` + `alloc` feature), Zig (`comptime`-safe)
- **Zero abstraction overhead** — compile-time backend dispatch via preprocessor; no vtables, no indirect calls
- **Two heap modes** — heap mode with `_create()` / `_destroy()`, and zero-heap mode using caller-supplied static storage via `_init()` / `_deinit()`
- **Kconfig-based configuration** — `make menuconfig` selects backend, board, and modules from a single `.config`

## Module umbrella

Include every module at once with:

```c
#include <ove/ove.h>
```

The complete module list with descriptions lives in the [API Reference Overview](api/index.md).

## Live demos

The example apps build to WebAssembly and run in a browser, served as part of the docs site:

- [C example](https://varcain.github.io/oveRTOS/example_c_heap/){:target="_blank"} · [C++ example](https://varcain.github.io/oveRTOS/example_cpp_heap/){:target="_blank"} · [Rust example](https://varcain.github.io/oveRTOS/example_rust_heap/){:target="_blank"} · [Zig example](https://varcain.github.io/oveRTOS/example_zig_heap/){:target="_blank"}
- [LVGL gallery (C++)](https://varcain.github.io/oveRTOS/lvgl_gallery_cpp_heap/){:target="_blank"}
- [Benchmarks](benchmarks/index.md) — same wrappers measured across all bindings

## When something goes wrong

- Run [`make doctor`](getting-started/doctor.md) first.
- Browse [Troubleshooting](getting-started/troubleshooting.md) for the most common failure modes.
- Check the [Glossary](glossary.md) if a piece of jargon is the part you're stuck on.
- File an issue: <https://github.com/Varcain/oveRTOS/issues>
