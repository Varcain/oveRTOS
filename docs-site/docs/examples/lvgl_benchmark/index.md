# LVGL Benchmark

A direct port of LVGL's upstream benchmark demo through the oveRTOS LVGL binding. Fifteen rendering scenes exercise widgets, animations, layout, image decoding, and compositing; a summary table reports per-scene FPS / CPU / render / flush time.

Source: `apps/{c,cpp,rust,zig}/heap/lvgl_benchmark/` (and matching `zeroheap/` variants).

## What it does

The app cycles through scenes automatically. Each scene runs for a fixed duration, during which LVGL's built-in profiling records:

| Metric | What it measures |
|---|---|
| **FPS** | Frames rendered per second |
| **CPU** | Percentage of CPU time spent in `lv_timer_handler` |
| **Render avg** | Mean render time per frame (μs) |
| **Flush avg** | Mean display flush time per frame (μs) |

After all scenes complete, a summary table replaces the scene area. The summary is the same across every language port — same scenes, same metric layout.

## Scenes

1. **Empty screen** — baseline.
2. **Rectangle** — solid fill.
3. **Rectangle, rounded** — anti-aliased corners.
4. **Rectangle, circle** — full circle.
5. **Rectangle, rounded, shadow** — drop shadow compositing.
6. **Rectangle, rounded, opaque, layer** — multi-layer composite.
7. **Rectangle, rounded, alpha 50, layer** — layer with transparency.
8. **Rectangle, image, RGB recolor** — image asset with palette swap.
9. **Image, ARGB recolor** — alpha-blended image.
10. **Image, ARGB shake** — animated alpha image.
11. **Label, small** — short text.
12. **Label, medium** — paragraph text.
13. **Label, large** — full-screen text.
14. **Substring, small** — partial redraw test.
15. **Substring, medium** — partial redraw with longer text.

The scene set tracks the upstream LVGL benchmark verbatim so numbers are directly comparable to other LVGL ports.

## Running it

```bash
# Host (POSIX) — opens the browser dashboard
make host.posix.lvgl_benchmark
make && make run

# QEMU + FreeRTOS — runs in the QEMU framebuffer
make qemu.freertos.lvgl_benchmark
make && make run

# STM32F7 Discovery board — flashes to hardware
make stm32f746.freertos.lvgl_benchmark
make && make flash
```

C++, Rust, and Zig ports build the same way — substitute `lvgl_benchmark_cpp`, `lvgl_benchmark_rust`, or `lvgl_benchmark_zig` for the third token.

## Per-language walkthroughs

- [C](c.md) — direct LVGL C API through `<ove/lvgl.h>`
- [C++](cpp.md) — RAII wrappers and typed widget builders
- [Rust](rust.md) — `no_std` safe wrappers via `ove::lvgl::*`
- [Zig](zig.md) — `comptime`-generic widget construction

## What to read it for

The benchmark is the easiest place to see:

- How the binding exposes LVGL's draw primitives (rectangles, shadows, animations).
- The pattern for **registering a scene** — a callback that builds widgets, plus a timer callback that swaps scenes.
- How the binding handles **image assets** (LVGL's `LV_IMAGE_DECLARE` macro in C; `lv_img_dsc_t` static descriptors in the higher-level languages).

## Performance notes

- The same scene on the same board produces near-identical numbers across language ports — the bindings compile down to identical LVGL calls. Spot-checks for any divergence > 5 % usually indicate a binding bug.
- The POSIX backend on a developer laptop yields wildly higher numbers than embedded targets; treat host runs as a regression baseline only.
- For headline numbers on STM32F746G-DISCO, see [Benchmarks → per-binding](../../benchmarks/per-binding.md).
