# oveRTOS Documentation

oveRTOS lets you **write embedded RTOS applications in C++, Rust, or Zig** and **run them unchanged on FreeRTOS, Zephyr, or Apache NuttX**. First-class language bindings sit on top of a unified application API — threads, synchronisation, networking (sockets/TLS/HTTP/MQTT), audio graph, ML inference, LVGL, power management — and the kernel underneath is a configure-time choice with [minimal runtime overhead](benchmarks/index.md). The pure C surface that the bindings target is available directly when you want it, but the higher-level bindings are the recommended path for application code. A POSIX backend runs the same source as a native Linux/macOS process; a WebAssembly backend renders it in a browser for live demos.

!!! tip "First time here?"
    Run through the [**Quickstart**](getting-started/quickstart.md) — three commands take you from a fresh clone to a running app in five minutes. No hardware required.

## Where to next

<div class="grid cards" markdown>

-   :material-rocket-launch: **Quickstart**

    ---

    `git clone` → `make setup` → `make doctor` → `make host.posix.example_cpp` → `make run`. The fastest path to a running oveRTOS app on your machine — pick C++, Rust, Zig, or C.

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

- **Modern-language application development** — C++ (RAII + typed containers), Rust (`no_std` + `alloc`), Zig (`comptime`-generic); the underlying C surface is available when you need it
- **Cross-RTOS portability** — same source on **FreeRTOS**, **Zephyr**, and **Apache NuttX**; backend chosen at configure time, no runtime indirection
- **Application-grade modules** — sockets / TLS / HTTP / MQTT / HTTPD / SNTP, audio graph engine, TFLite Micro inference, LVGL widgets, NVS, filesystem, power management, shell, watchdog, bus drivers
- **POSIX + WebAssembly development backends** — develop on Linux/macOS without hardware, demo in a browser
- **Two heap modes** — heap mode (`_create` / `_destroy`) and zero-heap mode (caller-supplied static storage via `_init` / `_deinit` or `OVE_*_DEFINE_STATIC`); both modes share the same source on every binding
- **Minimal abstraction overhead** — compile-time backend dispatch, no vtables, no indirect calls; per-binding wrapper cost is [measured on every commit](benchmarks/index.md)
- **Unified Kconfig** — `make menuconfig` selects binding, kernel, board, and modules from a single `.config`

## Pick your binding

Start with whichever fits your project — every binding compiles down to the same FFI symbols, with near-native overhead.

| Binding | Idiomatic style | When to pick |
|---|---|---|
| **C++** | RAII wrappers (`ove::Thread<N>`, `ove::Queue<T,N>`, `ove::Mutex`), typed containers, fluent LVGL builder | Existing C++ codebase, or you want destructor-driven cleanup with minimal ceremony |
| **Rust** | `no_std` crate, errors-as-values (`Result<T, Error>`), optional `alloc` feature for `Vec`/`Arc`, safe wrappers | Memory-safety guarantees matter; coming from server-side Rust |
| **Zig** | `comptime`-generic wrappers, `defer` for cleanup, `@hasDecl` feature gating | You want C-level visibility into what compiles, with stronger type safety |
| **C** | Direct `<ove/ove.h>`, `_create()` / `_destroy()` or `OVE_*_DEFINE_STATIC()` | The substrate — pick this for the smallest footprint, or to extend existing C firmware |

The same producer-consumer example exists in every binding under [`apps/{c,cpp,rust,zig}/`](https://github.com/Varcain/oveRTOS/tree/main/apps), and each has its own [walkthrough page](examples/example/index.md).

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
