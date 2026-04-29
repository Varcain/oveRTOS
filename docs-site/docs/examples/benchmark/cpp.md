# Benchmark — C++

Source: `tests/benchmarks/cpp/src/app.cpp` | **[WASM Demo](https://varcain.github.io/oveRTOS/benchmark_cpp/){:target="_blank"}**

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses C++20 RAII wrappers. Objects are declared at file scope — constructors handle initialization, destructors handle cleanup. Templates provide compile-time type safety.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.benchmark_cpp
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.benchmark_cpp
make configure && make download && make && make run
```
