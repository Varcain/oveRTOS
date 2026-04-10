# Keyword Detection (Live Audio + ML) — C

Source: `apps/c/example_keyword_live/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_keyword_live_c/){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses the unified C API with `_create()` / `_destroy()` calls that work in both heap and zero-heap modes.

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
