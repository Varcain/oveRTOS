# Keyword Detection (Live Audio + ML) — Rust

Source: `apps/rust/heap/example_keyword_live/src/lib.rs` (both modes) | **[Browser demo](demo:example_keyword_live_rust_heap){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the `ove` Rust crate with `no_std` support. Error handling uses `Result<T, ove::Error>` with per-op narrow error sets. The ring buffer indices and sample count use atomics. Both modes start the inference thread with `ove::thread!`. The macro uses the RTOS heap in heap mode and static storage in zero-heap mode. `#[ove::main]` exports the C-ABI entry symbol.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_keyword_live_rust
make configure && make download && make && make run
```

Audio node names are ordinary static Rust string literals, retained as native
counted spans without app C-string conversion or name allocation.
