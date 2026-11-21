# Keyword Detection (Live Audio + ML) — C

Source: `apps/c/heap/example_keyword_live/src/app.c` (both modes) | **[Browser demo](demo:example_keyword_live_heap){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

Both modes initialize a static audio graph with `ove_audio_graph_init()`. Heap mode creates the inference thread with `ove_thread_create()` and allocates graph buffers during `ove_audio_graph_build()`. Zero-heap mode uses `OVE_THREAD_DEFINE_STATIC()` and supplies buffers declared by `OVE_AUDIO_GRAPH_DEFINE()` through `ove_audio_graph_set_buf_storage()`.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_keyword_live
make configure && make download && make && make run

# Browser (the FreeRTOS firmware in QEMU compiled to WebAssembly)
make web.freertos.example_keyword_live
make configure && make download && make && make run
```
