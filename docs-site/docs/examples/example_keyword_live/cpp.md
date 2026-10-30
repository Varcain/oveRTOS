# Keyword Detection (Live Audio + ML) — C++

Source: `apps/cpp/example_keyword_live/src/app.cpp` | **[Browser demo](demo:example_keyword_live_cpp_heap){:target="_blank"}**

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language-specific patterns

This example uses C++20 RAII wrappers: constructors handle initialization, destructors handle cleanup, and templates provide compile-time type safety. Both models share one static `ove::ModelStorage<32768>`; each stage loads its model into it, and the session ends at the end of the stage, before the next model is loaded. The 32 KiB arena stays out of the inference thread's 8 KiB stack.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_keyword_live_cpp
make configure && make download && make && make run

# Browser (the FreeRTOS firmware in QEMU compiled to WebAssembly)
make web.freertos.example_keyword_live_cpp
make configure && make download && make && make run
```
