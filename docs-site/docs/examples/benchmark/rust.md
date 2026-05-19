# Benchmark — Rust

Source: `tests/benchmarks/rust/src/lib.rs` | **[WASM Demo](https://varcain.github.io/oveRTOS/benchmark_rust/){:target="_blank"}**

Measures latency, throughput, and memory usage of all RTOS abstractions.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<T, ove::Error>` with per-op narrow error sets. Shared state uses atomics; cross-thread kernel-object sharing uses `Arc<T>` in heap mode (from `ove::heap::Arc`) and `ove::shared!` cells (`InitCell<T>`) in zero-heap mode. `#[ove::main]` exports the C-ABI entry symbol.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.benchmark_rust
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.benchmark_rust
make configure && make download && make && make run
```
