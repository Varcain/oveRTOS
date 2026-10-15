# Networking Example — Rust

Source: `apps/rust/heap/example_net/src/lib.rs` (also `apps/rust/zeroheap/example_net/src/lib.rs`) | *No browser demo — the browser bundle has no network peer*

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<T, ove::Error>` with per-op narrow error sets. Shared state uses atomics; cross-thread kernel-object sharing uses `Arc<T>` in heap mode (from `ove::heap::Arc`) and `ove::shared!` cells (`InitCell<T>`) in zero-heap mode. `#[ove::main]` exports the C-ABI entry symbol.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_net_rust
make configure && make download && make && make run
```
