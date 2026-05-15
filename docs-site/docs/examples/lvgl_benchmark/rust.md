# LVGL Benchmark — Rust

Source: `apps/rust/heap/lvgl_benchmark/src/lib.rs` (and `apps/rust/zeroheap/lvgl_benchmark/`).

The Rust port uses the `ove::lvgl` module's safe wrappers (`Object`, `Label`, `Bar`, `Color`, `Layout`) with `ove::lvgl::lock()` returning an RAII guard. Scenes are stored as `&'static dyn Fn() -> ()` so the table compiles into ROM in zero-heap builds.

## Build

```bash
make host.posix.lvgl_benchmark_rust
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

Each builder is a free function (not a method) so the scene table is just an array of function pointers — no trait-object indirection, no allocator pressure.

## Scene table

```rust
struct Scene {
    name: &'static str,
    create: fn(),
    fps_avg: AtomicU32,
}

static SCENES: &[Scene] = &[
    Scene { name: "Empty screen",
            create: empty_create,             fps_avg: AtomicU32::new(0) },
    Scene { name: "Rectangle",
            create: rectangle_create,         fps_avg: AtomicU32::new(0) },
    Scene { name: "Rectangle, rounded",
            create: rectangle_rounded_create, fps_avg: AtomicU32::new(0) },
    /* … */
];
```

`AtomicU32` for the metrics so the scene-rotation closure can update them without a `Mutex`. In zero-heap mode the whole table lives in `.rodata` plus tiny `.data` for the atomics.

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

## no_std + alloc

The benchmark builds in both feature sets:

- **`default`** (no_std) — every scene table entry is `'static`, no `Box`, no `Arc`. Links against `ove-allocator` for any incidental `alloc` use inside `ove::lvgl`.
- **`std`** — the `ove-allocator` install is suppressed (`no_install`); the std allocator handles everything.

The same source file compiles for both feature sets without `cfg`-guards in the benchmark code itself.

## Where to next

- [Index](index.md)
- [C port](c.md) · [C++ port](cpp.md) · [Zig port](zig.md)
- [`bindings/rust/ove/src/lvgl.rs`](https://github.com/Varcain/oveRTOS/tree/main/bindings/rust/ove/src/lvgl.rs) — wrapper module
