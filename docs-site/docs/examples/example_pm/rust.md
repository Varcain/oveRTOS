# Power Management Example — Rust

Source: `apps/rust/example_pm/src/lib.rs` | *WASM demo not available*

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<(), ove::Error>`. Shared state uses atomics where possible. The `ove::shared!` macro provides safe static initialization.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_pm_rust
make configure && make download && make && make run
```
