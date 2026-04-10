# Keyword Detection (Live Audio + ML) — Zig

Source: `apps/zig/example_keyword_live/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the `ove` Zig module with comptime feature detection (`@hasDecl`), generic types, `defer`-based cleanup, and catch-based error handling.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_keyword_live_zig
make configure && make download && make && make run
```
