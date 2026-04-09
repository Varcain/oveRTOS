# Benchmark — Zig

Source: `apps/zig/benchmark/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses the `ove` Zig module with comptime feature detection (`@hasDecl`), generic types, `defer`-based cleanup, and catch-based error handling.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.benchmark_zig
make configure && make download && make && make run
```
