# LVGL Gallery — Rust

Source: `apps/rust/heap/lvgl_gallery/src/lib.rs`.

The Rust port uses `ove::lvgl::*` safe wrappers and `ove::lvgl::lock()` for the build phase. Widget text uses ordinary `&str`. The binding checks and converts it privately; formatted text uses `format_args!` and reports preparation failures through `Result`.

## Build

```bash
make qemu.freertos.lvgl_gallery_rust
make && make run
```

## Page builders

Each page is a plain `fn(Obj)` that builds widgets into the content area:

```rust
fn p_slider(c: Obj) {
    Slider::create(c)
        .size(300, 20)
        .range(0, 100)
        .value(50)
        .indicator_color(Color::hex(0x4CAF50))
        .center();
}
```

## Canvas pixels

The Canvas page asks the binding for native-owned pixels:

```rust
let canvas = Canvas::create(c)
    .size(32, 32)
    .allocate_buffer(32, 32, ColorFormat::RGB565)
    .expect("LVGL canvas pixels")
    .fill_bg(Color::hex(0x202020), 255);
canvas.set_pixel(1, 1, Color::white());
```

The RGB565 pixels occupy 2 KiB and use LVGL's pool in both heap modes. Pixel storage stays with the native
widget and is released when the widget tree is deleted. To initialize from
existing pixels, use
`canvas.set_buffer(CanvasBuffer::new(&pixels, width, height, format)?)?`.
The binding copies packed rows and handles native padding/alignment, so the
initializer can be reused or dropped after the call. I1 initializers include
eight palette bytes before their packed rows.

## Page table

```rust
type PageFn = fn(Obj);

static PAGES: &[(&str, PageFn)] = &[
    ("Label", p_label),
    ("Button", p_button),
    ("Slider", p_slider),
    /* … */
];
```

`'static` string names and function pointers: no allocator pressure for
the table itself. Showing a page builds it and puts its name in the title
label, formatted by the binding:

```rust
let name = PAGES[page].0;
lbl.set_text_fmt(format_args!("{} ({}/{})", name, nav.page + 1, N_PAGES))
    .expect("title text");
```

## Entry point

`app_main` builds the UI under `ove::lvgl::lock()`. A detached graphics
thread runs LVGL's tick and handler:

```rust
#[ove::main]
fn app_main() {
    ove::log::try_init();
    ove::Thread::builder()
        .name(c"graphics")
        .priority(Priority::High)
        .stack_size(16384)
        .spawn(|_tok| graphics_entry())
        .expect("graphics thread spawn")
        .detach();
    /* … create the UI, then */
    ove::run();
}
```

`#[ove::main]` is the proc-macro attribute (from `ove-macros`) that wraps `app_main` in the C-ABI `extern "C" fn ove_main()` symbol the framework links to. (Replaces the older `ove::main!(app_main)` declarative macro.)

## Cargo.toml

```toml
[dependencies]
ove = { path = "../../../../bindings/rust/ove", features = ["alloc"] }
ove-allocator = { path = "../../../../bindings/rust/ove-allocator",
                  default-features = false }
```

The LVGL widget wrappers come with the build's `CONFIG_OVE_LVGL`, not a Cargo feature.

## Where to next

- [Index](index.md)
- [C++ port](cpp.md) · [Zig port](zig.md)
- [`bindings/rust/ove/src/lvgl.rs`](https://github.com/Varcain/oveRTOS/tree/main/bindings/rust/ove/src/lvgl.rs)
