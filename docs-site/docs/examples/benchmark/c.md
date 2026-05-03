# Benchmark — C

Source: `tests/benchmarks/c/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/benchmark/){:target="_blank"}**

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses the heap-mode C API with `_create()` / `_destroy()` calls. For zero-heap builds, switch to `_init()` / `_deinit()` with caller-supplied storage or use `OVE_*_DEFINE_STATIC()` at file scope (the latter also works in heap mode).

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
