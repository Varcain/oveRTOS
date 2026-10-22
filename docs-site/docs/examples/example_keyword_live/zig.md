# Keyword Detection (Live Audio + ML) — Zig

Source: `apps/zig/heap/example_keyword_live/src/main.zig` (also `apps/zig/zeroheap/example_keyword_live/src/main.zig`) | **[Browser demo](demo:example_keyword_live_zig_heap){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the `ove` Zig module with allocator-aware constructors (`Type.create(allocator)`), exhaustive RTOS dispatch via `switch (ove.target.current_rtos)`, narrow per-op error sets, `defer`-based cleanup, and `std.log.*` integration through `ove.log.logFn`.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_keyword_live_zig
make configure && make download && make && make run
```
