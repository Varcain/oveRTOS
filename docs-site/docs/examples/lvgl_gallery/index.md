# LVGL Gallery

A one-widget-per-page tour of every wrapped LVGL widget in oveRTOS. Top nav bar walks through the pages; each page exercises one widget type at full size with its typical configuration.

Source: `apps/{cpp,rust,zig}/heap/lvgl_gallery/`. **No C port** — the gallery is the showcase for the typed wrappers, which the C port wouldn't add anything to.

## Why no C variant

The gallery's purpose is to show the binding's *ergonomics* — the fluent style builder, the typed widget hierarchy, the RAII patterns. The C source would just be a procession of `lv_*` calls indistinguishable from the upstream LVGL examples. Run the LVGL upstream demo if you want raw-C; run the gallery if you want to gauge the binding.

## Widgets covered

22 widget types, one per page:

- **Basics**: Object, Label, Image, Button, Image Button
- **Inputs**: Slider, Switch, Roller, Spinbox, Keyboard
- **Containers**: Tile View, Tab View, Window, Menu
- **Indicators**: Bar, Arc, Spinner, LED, Meter
- **Data**: Table, Chart, Calendar, List

The list tracks what's wrapped in `bindings/cpp/ove/lvgl.hpp` — if a widget is missing here, it's missing from the binding.

## Running it

```bash
# C++ port
make host.posix.lvgl_gallery_cpp
make && make run

# Rust port
make host.posix.lvgl_gallery_rust
make && make run

# Zig port
make host.posix.lvgl_gallery_zig
make && make run
```

On embedded targets (`qemu.freertos.lvgl_gallery_cpp`, `stm32f746.freertos.lvgl_gallery_cpp`, etc.) the build is identical — the same gallery code lights up the QEMU framebuffer or the Discovery board's display.

## Live demo

The C++ gallery builds to WebAssembly: [varcain.github.io/oveRTOS/lvgl_gallery_cpp_heap/](https://varcain.github.io/oveRTOS/lvgl_gallery_cpp_heap/){:target="_blank"}.

## Per-language walkthroughs

- [C++](cpp.md) — `ove::lvgl::*` typed wrappers, fluent style builder
- [Rust](rust.md) — safe wrappers + alloc feature for widget storage
- [Zig](zig.md) — comptime widget construction, `defer`-based teardown

## What to read it for

The gallery is the easiest way to:

- See **every wrapped widget** and the canonical way to configure it in your language of choice.
- Compare **API ergonomics** across the three high-level bindings on identical functionality.
- Find **starter code** to copy — each page is a complete widget instantiation.

If you want to know whether oveRTOS's binding exposes a particular LVGL feature, search this app first. If you can build it here, you can build it in your own app.
