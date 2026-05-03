# Examples

oveRTOS ships with five example applications, each available in C, C++, Rust, and Zig. All examples compile and run identically across FreeRTOS, Zephyr, NuttX, and POSIX backends.

Interactive WASM demos run directly in your browser — no toolchain needed.

| Example | Description | APIs Demonstrated |
|---------|-------------|-------------------|
| [Basic Example](example/index.md) | Producer-consumer with LVGL display | Threads, queues, mutexes, timers, LVGL |
| [Benchmark](benchmark/index.md) | Latency and throughput measurements | All RTOS primitives, memory stats |
| [Networking](example_net/index.md) | Full network stack test suite | Sockets, DNS, HTTP, MQTT, SNTP, HTTPD |
| [Power Management](example_pm/index.md) | Sleep states, domains, wake sources | PM state machine, domains, policy, stats |
| [Keyword Detection](example_keyword_live/index.md) | Real-time "yes/no" speech recognition | Audio graph, ML inference, I2S |

### Additional LVGL demos

Two LVGL-focused apps live under `apps/<lang>/` and are not documented per-page — they reimplement standard LVGL demos through the oveRTOS bindings:

- **`lvgl_benchmark`** (C / C++ / Rust / Zig) — port of the upstream LVGL benchmark scenes. 15 rendering scenes stress-test widgets, animations, layout, images, and compositing; reports per-scene FPS / CPU / render / flush time. Build with `make host.posix.lvgl_benchmark[_cpp|_rust|_zig]`.
- **`lvgl_gallery`** (C++ / Rust / Zig — no C variant) — one widget per page with a top nav bar, exercising all 22 widget types in the LVGL bindings. Build with `make host.posix.lvgl_gallery_cpp[_rust|_zig]`.

## Language support

Each example is implemented in all four languages using the same oveRTOS API:

- **C** — `_create()` / `_destroy()` (heap mode) or `OVE_*_DEFINE_STATIC()` / `_init()` (works in both modes)
- **C++** — RAII wrappers (`ove::Thread<4096>`, `ove::Queue<T, N>`), template patterns
- **Rust** — `no_std` crate with `Result` error handling, safe abstractions over FFI
- **Zig** — `@cImport` bindings, comptime feature detection, `defer` cleanup

## WASM demos

Most examples have browser-based WASM demos. Click the links on each example's page to try them live.

| Language | WASM support |
|----------|-------------|
| C | All examples |
| C++ | All examples |
| Rust | Basic, Benchmark, Networking |
| Zig | Not yet supported (Zig 0.15 lacks wasm32-emscripten) |
