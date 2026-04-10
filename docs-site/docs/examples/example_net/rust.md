# Networking Example — Rust

Source: `apps/rust/example_net/src/lib.rs` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_net_rust/){:target="_blank"}**

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<(), ove::Error>`. Shared state uses atomics where possible. The `ove::shared!` macro provides safe static initialization.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_net_rust
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.example_net_rust
make configure && make download && make && make run
```
