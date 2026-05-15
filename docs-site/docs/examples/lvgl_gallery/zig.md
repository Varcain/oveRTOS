# LVGL Gallery — Zig

Source: `apps/zig/heap/lvgl_gallery/src/main.zig`.

The Zig port uses `ove.lvgl` typed wrappers and `defer guard.deinit()` for the build-phase lock. Page tables are `comptime` arrays of function pointers.

## Build

```bash
make host.posix.lvgl_gallery_zig
make && make run
```

## Page builders

```zig
fn buildSliderPage(parent: lvgl.Object) void {
    var slider = lvgl.Slider.new(parent);
    slider.setSize(200, 12);
    slider.center();
    slider.setRange(0, 100);
    slider.setValue(42);
    slider.onChange(struct {
        fn cb(v: i32) void { _ = v; /* … */ }
    }.cb);
}
```

Closures aren't first-class in Zig the way they are in Rust, but a nested `struct { fn cb(...) }.cb` pattern produces an equivalent function pointer at comptime with no allocator involvement.

## Page table

```zig
const Page = struct {
    name: []const u8,
    build: *const fn (lvgl.Object) void,
};

const pages = [_]Page{
    .{ .name = "Object", .build = buildObjectPage },
    .{ .name = "Label",  .build = buildLabelPage  },
    .{ .name = "Slider", .build = buildSliderPage },
    /* … */
};
```

`comptime` array — sits in `.rodata`.

## Entry point

```zig
fn appMain() void {
    var guard = ove.lvgl.lock();
    defer guard.deinit();

    const screen = lvgl.Object.activeScreen();
    var tabs = lvgl.TabView.new(screen, lvgl.Direction.top, 40);
    inline for (pages) |p| {
        const tab = tabs.addTab(p.name);
        p.build(tab);
    }
}

comptime { ove.exportMain(appMain); }
```

`inline for` unrolls the loop at comptime — each page builder is called directly, no function-pointer dispatch through the table. The table itself stays for readability.

## Comptime widget gating

If the LVGL config disables a particular widget (`CONFIG_OVE_LVGL_USE_CHART=n`), the corresponding page can be omitted at comptime:

```zig
if (comptime ove.lvgl.has(.Chart)) {
    .{ .name = "Chart", .build = buildChartPage },
} else {
    .{ .name = "Chart (disabled)", .build = buildDisabledStub },
}
```

`ove.lvgl.has(.X)` checks `@hasDecl` against the bindings stub — false-branch code never gets emitted.

## Where to next

- [Index](index.md)
- [C++ port](cpp.md) · [Rust port](rust.md)
- [`bindings/zig/ove/`](https://github.com/Varcain/oveRTOS/tree/main/bindings/zig/ove)
