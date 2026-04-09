# Keyword Detection (Live Audio + ML) — Rust

Source: `apps/rust/example_keyword_live/src/lib.rs` | *WASM demo not available*

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<(), ove::Error>`. Shared state uses atomics where possible. The `ove::shared!` macro provides safe static initialization.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_keyword_live_rust
make configure && make download && make && make run
```
