# Keyword Detection (Live Audio + ML) — Zig

Source: `apps/zig/heap/example_keyword_live/src/main.zig` (both modes) | **[Browser demo](demo:freertos/example_keyword_live_zig_heap){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

The inference thread uses `ove.allocators.c_allocator` in heap mode and a fixed-buffer allocator in zero-heap mode. Heap mode calls `ove.Model.create`. Zero-heap mode calls `Model.init` with a static model arena and supplies static audio graph buffers.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_keyword_live_zig
make configure && make download && make && make run
```
