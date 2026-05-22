# Keyword Detection (Live Audio + ML) — Zig

Source: `apps/zig/heap/example_keyword_live/src/main.zig` (also `apps/zig/zeroheap/example_keyword_live/src/main.zig`) | *WASM demo not available — Zig 0.16 still lacks wasm32-emscripten support*

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the `ove` Zig module with allocator-aware constructors (`Type.create(allocator)`), exhaustive RTOS dispatch via `switch (ove.target.current_rtos)`, narrow per-op error sets, `defer`-based cleanup, and `std.log.*` integration through `ove.log.logFn`.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_keyword_live_zig
make configure && make download && make && make run
```
