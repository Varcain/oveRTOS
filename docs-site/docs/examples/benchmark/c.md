# Benchmark — C

Source: `apps/c/benchmark/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/benchmark/){:target="_blank"}**

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses the unified C API with `_create()` / `_destroy()` calls that work in both heap and zero-heap modes.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.benchmark
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.benchmark
make configure && make download && make && make run
```
