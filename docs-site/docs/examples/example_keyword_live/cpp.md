# Keyword Detection (Live Audio + ML) — C++

Source: `apps/cpp/example_keyword_live/src/app.cpp` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_keyword_live_cpp/){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses C++20 RAII wrappers. Objects are declared at file scope — constructors handle initialization, destructors handle cleanup. Templates provide compile-time type safety.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_keyword_live_cpp
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.example_keyword_live_cpp
make configure && make download && make && make run
```
