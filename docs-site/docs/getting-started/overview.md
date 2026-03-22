# Architecture Overview

## What oveRTOS Does

oveRTOS is an embedded RTOS framework with three main parts: a build system that manages RTOS sources, toolchains, and configuration; a portable API layer that maps application calls to the selected backend at compile time; and language bindings that expose the API in C++, Rust, and Zig alongside the core C interface. The build system downloads RTOS kernel sources, acquires cross-compilation toolchains, and translates a single Kconfig-based `.config` into the native configuration format of each RTOS. The API layer resolves every public call directly to the backend implementation at compile time — there are no virtual functions, no function pointer tables, and no branch overhead at runtime. LVGL is integrated as the GUI toolkit, with display and input drivers provided for each backend.

## Layer Diagram

```mermaid
flowchart TD
    A["<b>Application Code</b><br>C · C++ · Rust · Zig"]
    B["<b>oveRTOS API Layer</b><br>thread · sync · queue · timer · audio · fs · lvgl …"]
    C["<b>CONFIG_OVE_RTOS_*</b><br>compile-time dispatch via Kconfig"]

    A --> B --> C

    C --> D & E & F & G

    subgraph backends [" "]
        direction LR
        D["FreeRTOS<br>backend"] --> H["FreeRTOS<br>kernel"]
        E["NuttX<br>backend"] --> I["NuttX<br>kernel"]
        F["Zephyr<br>backend"] --> J["Zephyr<br>kernel"]
        G["POSIX<br>backend"] --> K["pthreads<br>SDL2"]
    end

    style A fill:#5c6bc0,stroke:#3949ab,color:#fff
    style B fill:#1e88e5,stroke:#1565c0,color:#fff
    style C fill:#f57c00,stroke:#e65100,color:#fff
    style D fill:#43a047,stroke:#2e7d32,color:#fff
    style E fill:#43a047,stroke:#2e7d32,color:#fff
    style F fill:#43a047,stroke:#2e7d32,color:#fff
    style G fill:#43a047,stroke:#2e7d32,color:#fff
    style H fill:#546e7a,stroke:#37474f,color:#fff
    style I fill:#546e7a,stroke:#37474f,color:#fff
    style J fill:#546e7a,stroke:#37474f,color:#fff
    style K fill:#546e7a,stroke:#37474f,color:#fff
    style backends fill:none,stroke:none
```

## Build System

The `ove` CLI handles the full build lifecycle:

| Phase | Command | What it does |
|-------|---------|--------------|
| Download | `make download` | Fetches RTOS kernel sources (FreeRTOS, NuttX, Zephyr) and cross-compilation toolchains into `dl/` |
| Configure | `make configure` | Reads `.config` and generates RTOS-native config files (`FreeRTOSConfig.h`, `prj.conf`, NuttX defconfig) plus framework headers |
| Build | `make build` | Invokes each RTOS's native build system (CMake for FreeRTOS/POSIX, `west build` for Zephyr, `make` for NuttX) |
| Run | `make run` | Launches firmware on QEMU or as a native POSIX process |
| Flash | `make flash` | Programs hardware via OpenOCD or the backend's flash runner |

RTOS source URLs, versions, and download methods are configurable via Kconfig (`Config.in.rtos`). Each RTOS also exposes its own native menuconfig through `make nuttx-menuconfig` or `make zephyr-menuconfig`, with a layered config system that preserves user customisations across rebuilds.

## Compile-Time Dispatch

The active backend is selected by a single `CONFIG_OVE_RTOS_*` symbol written into `ove_config.h` by Kconfig. Each backend directory provides its own implementation files for every oveRTOS module. The C preprocessor picks the right source tree at build time — there is no runtime branch on the backend identity.

Because the choice is entirely preprocessor-driven, the compiler sees exactly one implementation per function and inlines or optimises it as it would any other C code. The abstraction costs zero cycles at runtime.

## Heap Mode vs Zero-Heap Mode

Every oveRTOS object type has two sets of lifecycle functions:

| Mode | Create | Destroy | Memory |
|---|---|---|---|
| **Heap mode** (default) | `ove_thread_create(...)` | `ove_thread_destroy(t)` | Allocated from RTOS heap |
| **Zero-heap mode** | `ove_thread_init(buf, ...)` | `ove_thread_deinit(buf)` | Caller-supplied static buffer |

Zero-heap mode is enabled with `CONFIG_OVE_ZERO_HEAP=y` in Kconfig. When set, the `_create()` and `_destroy()` family of functions are compiled out entirely. This is suitable for safety-critical or resource-constrained targets where dynamic allocation must be provably absent.

## Backend Model

| Backend | Config symbol | Typical use |
|---|---|---|
| FreeRTOS | `CONFIG_OVE_RTOS_FREERTOS` | STM32 hardware targets via STM32CubeF7 |
| Apache NuttX | `CONFIG_OVE_RTOS_NUTTX` | POSIX-compliant embedded systems |
| Zephyr RTOS | `CONFIG_OVE_RTOS_ZEPHYR` | Broad hardware support via West |
| POSIX/SDL2 | `CONFIG_OVE_RTOS_POSIX` | Native Linux/macOS development and testing |

The POSIX backend runs natively on the host with pthreads for concurrency and SDL2 for display and audio emulation — no cross-compilation or hardware required.

## Language Bindings

oveRTOS ships first-class bindings in four languages. All bindings target the same underlying C API and are built automatically by the framework's build system:

- **C** — direct use of `<ove/ove.h>`
- **C++** — RAII wrappers with move semantics, compile-time stack sizing, and fluent LVGL widget builders (`bindings/cpp/`)
- **Rust** — `no_std` crate with safe thread entry functions, error handling via `Result<T, Error>`, and LVGL bindings (`bindings/rust/ove/`)
- **Zig** — comptime-safe wrappers with generic `Queue(T, N)` types, `@hasDecl`-based feature detection, and LVGL bindings (`bindings/zig/ove/`)

## LVGL Integration

The [LVGL](https://lvgl.io) graphics library is downloaded, configured, and built as part of the oveRTOS framework. Each backend provides a display driver and tick source:

| Backend | Display driver | Input |
|---------|---------------|-------|
| FreeRTOS (STM32) | Hardware LCD via LTDC/DMA2D | Touch controller |
| Zephyr | Zephyr display subsystem | Zephyr input subsystem |
| NuttX | NuttX framebuffer (`/dev/fb0`) | NuttX input |
| POSIX | SDL2 window | SDL2 mouse/keyboard |
| QEMU | Emulated framebuffer | — |

Thread safety is handled through `ove_lvgl_lock()` / `ove_lvgl_unlock()`, which each language binding wraps in its idiomatic RAII pattern (`LvglGuard` in C++, `lvgl::lock()` guard in Rust, `defer guard.deinit()` in Zig). LVGL is enabled via `CONFIG_OVE_LVGL` in Kconfig.
