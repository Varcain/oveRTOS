# LVGL Benchmark — Rust

Source: `apps/rust/heap/lvgl_benchmark/src/lib.rs` (both modes).

The Rust port uses the `ove::lvgl` module's safe wrappers (`Object`, `Label`, `Bar`, `Color`, `Layout`) with `ove::lvgl::lock()` returning an RAII guard.

## Build

```bash
make qemu.freertos.lvgl_benchmark_rust
make && make run
```

## Scene definition

```rust
fn rectangle_rounded_create() {
    let scr = lvgl::Object::active_screen();
    let obj = lvgl::Object::new(scr);
    obj.set_size(200, 200);
    obj.center();
    obj.style().background_color(Color::hex(0xff_00_00)).radius(20);
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

```rust
fn build_card() {
    let _guard = ove::lvgl::lock();
    let card = ove::lvgl::Object::new(parent);
    card.set_size(120, 80);
    /* lock released on _guard drop */
}
```

The benchmark itself runs everything from the LVGL handler thread (which already holds the lock internally for the duration of `lv_timer_handler`), so explicit locking only matters if you drive scenes from a worker thread.

## Memory modes

Both firmware modes use the same `no_std` crate without the `alloc` feature
or a Rust global allocator. `ove::thread!` starts the graphics worker from
the RTOS heap in heap mode and from static storage in zero-heap mode.

## Where to next

- [Index](index.md)
- [C port](c.md) · [C++ port](cpp.md) · [Zig port](zig.md)
- [`bindings/rust/ove/src/lvgl.rs`](https://github.com/Varcain/oveRTOS/tree/main/bindings/rust/ove/src/lvgl.rs) — wrapper module
