# Keyword Detection (Live Audio + ML) — C

Source: `apps/c/example_keyword_live/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_keyword_live_heap/){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the heap-mode C API with `_create()` / `_destroy()` calls. For zero-heap builds, switch to `_init()` / `_deinit()` with caller-supplied storage or use `OVE_*_DEFINE_STATIC()` at file scope (the latter also works in heap mode).

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_keyword_live
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.example_keyword_live
make configure && make download && make && make run
```
