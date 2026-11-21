# LVGL Benchmark — Rust

Source: `apps/rust/heap/lvgl_benchmark/src/lib.rs` (both modes).

The Rust port uses safe `ove::lvgl` wrappers such as `Obj`, `Label`, `Slider`,
and `Color`, along with the `Layout` trait. `ove::lvgl::lock()` returns an RAII
guard.

## Build

```bash
make qemu.freertos.lvgl_benchmark_rust
make && make run
```

## Scene definition

The single-rectangle scene uses the benchmark's `LV_OPA_COVER` constant and
its `lv_pct` and `color_anim` helpers.

```rust
fn single_rectangle_cb() {
    let scr = lvgl::screen_active();
    let obj = Obj::create(scr);
    obj.remove_style_all();
    obj.bg_opa(LV_OPA_COVER)
        .center()
        .size(lv_pct(30), lv_pct(30));
    color_anim(obj);
}
```

Each scene descriptor holds a function pointer to the builder and a duration in milliseconds.

## Scene table

```rust
#[derive(Clone, Copy)]
struct SceneDsc {
    name: &'static str,
    create_cb: fn(),
    scene_time: u32,
}

static SCENES: [SceneDsc; 17] = [
    SceneDsc::new("Empty screen", empty_screen_cb, 3000),
    SceneDsc::new("Moving wallpaper", moving_wallpaper_cb, 3000),
    SceneDsc::new("Single rectangle", single_rectangle_cb, 3000),
    // Remaining scenes omitted.
];
```

The scene table lives in flash in both modes, and `STATS` holds per-scene statistics in an `LvCell<[SceneStats; 17]>`.

## Locking

To create the benchmark's card panel under the LVGL lock, keep the guard in
scope until the panel is configured.

```rust
{
    let _guard = lvgl::lock();
    let scr = lvgl::screen_active();
    let panel = Obj::create(scr);
    panel.size(270, 120).pad_all(8);
}
```

The benchmark acquires the lock during initial setup and around `lvgl::tick()`
and `lvgl::handler()` on the graphics worker. Scene callbacks invoked by the
handler run while that guard is held.

## Memory modes

Both firmware modes use the same `no_std` crate without the `alloc` feature
or a Rust global allocator. `ove::thread!` starts the graphics worker from
the RTOS heap in heap mode and from static storage in zero-heap mode.

## Where to next

- [Index](index.md)
- [C port](c.md) · [C++ port](cpp.md) · [Zig port](zig.md)
- [`bindings/rust/ove/src/lvgl.rs`](https://github.com/Varcain/oveRTOS/tree/main/bindings/rust/ove/src/lvgl.rs) — wrapper module
