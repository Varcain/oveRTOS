# LVGL Benchmark — C++

Source: `apps/cpp/heap/lvgl_benchmark/src/app.cpp` (and `apps/cpp/zeroheap/lvgl_benchmark/`).

The C++ port uses oveRTOS's typed LVGL wrappers (`ove::lvgl::*`) for widget construction and `ove::LvglGuard` for RAII locking. Scenes still hit upstream LVGL types where the wrapper doesn't add value (`lv_color_hex`, animation primitives).

## Build

```bash
make host.posix.lvgl_benchmark_cpp
make qemu.freertos.lvgl_benchmark_cpp
make && make run
```

## Scene construction with the typed wrapper

A scene that builds a rectangle in C++:

```cpp
static void rectangle_rounded_create()
{
    auto obj = ove::lvgl::Object(lv_screen_active());
    obj.size(200, 200).center();
    obj.style().background_color(0xff0000).radius(20);
}
```

`ove::lvgl::Object` wraps `lv_obj_t *`; `.style()` returns a fluent builder for the common style properties. Destructor is intentionally a no-op — LVGL owns the lifetime, the wrapper is a thin handle.

## Scene table

```cpp
struct Scene {
    const char *name;
    void (*create)();
    uint32_t fps_avg;   /* filled by next_scene_cb */
    /* … */
};

static Scene scenes[] = {
    { "Empty screen",        empty_create,             0 },
    { "Rectangle",           rectangle_create,         0 },
    { "Rectangle, rounded",  rectangle_rounded_create, 0 },
    /* … */
};
```

Same shape as the C port — `auto` and member functions keep call sites compact.

## RAII locking

If you adapt the benchmark to drive a scene from a worker thread, the lock is scope-bound:

```cpp
{
    ove::LvglGuard guard;
    auto card = ove::lvgl::Object(parent);
    card.size(120, 80).align(LV_ALIGN_CENTER);
}   /* guard destructor releases lock */
```

`ove::LvglGuard` is move-deleted; you can't accidentally extend its lifetime by passing it across functions.

## Templates and `constexpr`

Widget builders that take dimensions can be templated when the size is compile-time known:

```cpp
template <int W, int H>
static auto card_create(auto parent)
{
    auto card = ove::lvgl::Object(parent);
    card.size(W, H);
    return card;
}
```

The benchmark itself doesn't lean on this (scene sizes are deliberately runtime), but the LVGL gallery does.

## Where to next

- [Index](index.md) — what the benchmark measures
- [C port](c.md) — same scenes through the raw LVGL C API
- [Rust port](rust.md) · [Zig port](zig.md)
- [`bindings/cpp/ove/lvgl.hpp`](https://github.com/Varcain/oveRTOS/tree/main/bindings/cpp/ove/lvgl.hpp) — the wrapper surface
