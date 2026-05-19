# Benchmark — Zig

Source: `tests/benchmarks/zig/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses the `ove` Zig module with allocator-aware constructors (`Type.create(allocator)`), exhaustive RTOS dispatch via `switch (ove.target.current_rtos)`, narrow per-op error sets, `defer`-based cleanup, and `std.log.*` integration through `ove.log.logFn`.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.benchmark_zig
make configure && make download && make && make run
```
