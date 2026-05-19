# Power Management Example — Rust

Source: `apps/rust/heap/example_pm/src/lib.rs` (also `apps/rust/zeroheap/example_pm/src/lib.rs`) | *WASM demo not available*

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<T, ove::Error>` with per-op narrow error sets (e.g. `try_send_for` returns only `Timeout`/`QueueFull`). Shared state uses atomics; cross-thread kernel-object sharing uses `Arc<T>` in heap mode (from `ove::heap::Arc`) and `ove::shared!` cells (`InitCell<T>`) in zero-heap mode. `#[ove::main]` exports the C-ABI entry symbol.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_pm_rust         # heap mode
make host.posix.example_pm_rust_zh      # zero-heap mode
make configure && make download && make && make run
```
