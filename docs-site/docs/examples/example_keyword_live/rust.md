# Keyword Detection (Live Audio + ML) — Rust

Source: `apps/rust/heap/example_keyword_live/src/lib.rs` (also `apps/rust/zeroheap/example_keyword_live/src/lib.rs`) | *WASM demo not available*

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<T, ove::Error>` with per-op narrow error sets. Shared state uses atomics; cross-thread kernel-object sharing uses `Arc<T>` in heap mode (from `ove::heap::Arc`) and `ove::shared!` cells (`InitCell<T>`) in zero-heap mode. `#[ove::main]` exports the C-ABI entry symbol.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_keyword_live_rust
make configure && make download && make && make run
```
