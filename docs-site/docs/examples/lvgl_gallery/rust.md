# LVGL Gallery — Rust

Source: `apps/rust/heap/lvgl_gallery/src/lib.rs`.

The Rust port uses `ove::lvgl::*` safe wrappers and `ove::lvgl::lock()` for the build phase. The `alloc` feature is enabled so that the `Vec<Page>` storage and any closure-based callbacks (`on_change`, `on_click`) can be allocated on the heap.

## Build

```bash
make host.posix.lvgl_gallery_rust
make && make run
```

## Page builders

```rust
fn build_slider_page(parent: &lvgl::Object) {
    let slider = lvgl::Slider::new(parent);
    slider.set_size(200, 12).center();
    slider.set_range(0, 100).set_value(42);
    slider.on_change(|v| { /* … */ });
}
```

The `on_change` closure captures by move; the binding boxes it and stores the box inside the LVGL widget's user data. Drop ordering: dropping the wrapper unregisters the callback before freeing the box, so there's no use-after-free.

## Page table

```rust
struct Page {
    name: &'static str,
    build: fn(&lvgl::Object),
}

static PAGES: &[Page] = &[
    Page { name: "Object", build: build_object_page },
    Page { name: "Label",  build: build_label_page  },
    Page { name: "Slider", build: build_slider_page },
    /* … */
];
```

`'static` references everywhere — no allocator pressure for the table itself.

## Entry point

```rust
fn app_main() {
    let _guard = ove::lvgl::lock();
    let screen = lvgl::Object::active_screen();
    let tabs = lvgl::TabView::new(&screen, lvgl::Direction::Top, 40);
    for page in PAGES {
        let tab = tabs.add_tab(page.name);
        (page.build)(&tab);
    }
    drop(_guard);   /* release before run() */
    ove::run();
}

ove::main!(app_main);
```

`ove::main!` exports the `ove_main` symbol with the right C ABI; without it the framework can't find the entry point.

## Cargo.toml

```toml
[dependencies]
ove = { path = "../../../../bindings/rust/ove",
        features = ["alloc", "lvgl"] }
ove-allocator = { path = "../../../../bindings/rust/ove-allocator",
                  default-features = false }
```

The `alloc` feature is required for closure capture; the `lvgl` feature pulls in the widget wrappers.

## Where to next

- [Index](index.md)
- [C++ port](cpp.md) · [Zig port](zig.md)
- [`bindings/rust/ove/src/lvgl.rs`](https://github.com/Varcain/oveRTOS/tree/main/bindings/rust/ove/src/lvgl.rs)
