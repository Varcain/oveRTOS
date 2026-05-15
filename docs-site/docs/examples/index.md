# Examples

oveRTOS ships with four example applications, each available in C, C++, Rust, and Zig. All examples compile and run identically across FreeRTOS, Zephyr, NuttX, and POSIX backends.

Interactive WASM demos run directly in your browser — no toolchain needed.

| Example | Description | APIs Demonstrated |
|---------|-------------|-------------------|
| [Basic Example](example/index.md) | Producer-consumer with LVGL display | Threads, queues, mutexes, timers, LVGL |
| [Networking](example_net/index.md) | Full network stack test suite | Sockets, DNS, HTTP, MQTT, SNTP, HTTPD |
| [Power Management](example_pm/index.md) | Sleep states, domains, wake sources | PM state machine, domains, policy, stats |
| [Keyword Detection](example_keyword_live/index.md) | Real-time "yes/no" speech recognition | Audio graph, ML inference, I2S |
| [LVGL Benchmark](lvgl_benchmark/index.md) | Upstream LVGL benchmark scenes through the binding | LVGL widgets, animations, image decoding, FPS/CPU/render metrics |
| [LVGL Gallery](lvgl_gallery/index.md) | One-widget-per-page tour of every wrapped LVGL widget | LVGL widget surface (22 types), typed wrappers, RAII locking |

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
| Rust | Basic, Networking |
| Zig | Not yet supported (Zig 0.15 lacks wasm32-emscripten) |
