# Keyword Detection (Live Audio + ML)

Real-time 'yes'/'no' keyword detection using on-board microphones and the TensorFlow Lite Micro micro_speech model. Audio is captured via I2S DMA, spectral features are extracted, and a neural network classifier runs inference every ~1 second.

## Language implementations

| Language | Source | WASM Demo |
|----------|--------|-----------|
| [C](c.md) | `apps/c/example_keyword_live/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_keyword_live_c/){:target="_blank"}** |
| [C++](cpp.md) | `apps/cpp/example_keyword_live/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_keyword_live_cpp/){:target="_blank"}** |
| [Rust](rust.md) | `apps/rust/example_keyword_live/` | *Not available* |
| [Zig](zig.md) | `apps/zig/example_keyword_live/` | *Not yet available* |

## Key APIs demonstrated

ove_audio_graph_*, ove_audio_device_source/sink, ove_model_init, ove_model_invoke, ove_model_input/output, ove_model_deinit

## Required Kconfig

`CONFIG_OVE_BSP=y, CONFIG_OVE_AUDIO=y, CONFIG_OVE_AUDIO_NODE_CONVERTER=y, CONFIG_OVE_INFER=y`

## How to build

```bash
make host.posix.example_keyword_live      # C
make host.posix.example_keyword_live_cpp  # C++
make configure && make download && make && make run
```
