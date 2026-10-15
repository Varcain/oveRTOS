# Benchmark — C

Source: `tests/benchmarks/c/src/app.c` | *No browser demo — the benchmark times itself with the DWT cycle counter, which QEMU does not model*

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses the heap-mode C API with `_create()` / `_destroy()` calls. For zero-heap builds, switch to `_init()` / `_deinit()` with caller-supplied storage or use `OVE_*_DEFINE_STATIC()` at file scope (the latter also works in heap mode).

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.benchmark
make configure && make download && make && make run
```
