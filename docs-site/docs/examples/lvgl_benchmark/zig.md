# LVGL Benchmark — Zig

Source: `apps/zig/heap/lvgl_benchmark/src/main.zig` (and `apps/zig/zeroheap/lvgl_benchmark/`).

The Zig port uses `ove.lvgl` for widget construction. Scene tables are `comptime` arrays of function pointers; metrics are stored in `std.atomic.Value(u32)`.

## Build

```bash
make host.posix.lvgl_benchmark_zig
make qemu.freertos.lvgl_benchmark_zig
make && make run
```

## Scene builders

```zig
fn rectangleRoundedCreate() void {
    const scr = lvgl.Object.activeScreen();
    var obj = lvgl.Object.new(scr);
    obj.setSize(200, 200);
    obj.center();
    obj.style().backgroundColor(lvgl.Color.hex(0xff_00_00)).radius(20);
}
```

`var obj = ...` because the fluent style chain mutates `obj.style()` results. Zig's value semantics keep the wrapper a thin handle (a pointer plus a phantom for borrow checking).

## Scene table

```zig
const Scene = struct {
    name: []const u8,
    create: *const fn () void,
    fps_avg: std.atomic.Value(u32),
};

var scenes = [_]Scene{
    .{ .name = "Empty screen",
       .create = emptyCreate,
       .fps_avg = std.atomic.Value(u32).init(0) },
    .{ .name = "Rectangle",
       .create = rectangleCreate,
       .fps_avg = std.atomic.Value(u32).init(0) },
    .{ .name = "Rectangle, rounded",
       .create = rectangleRoundedCreate,
       .fps_avg = std.atomic.Value(u32).init(0) },
    // …
};
```

`comptime`-known length so the array goes to `.data` directly; no allocator involved.

## Locking

```zig
fn buildCard() void {
    var guard = ove.lvgl.lock();
    defer guard.deinit();

    var card = ove.lvgl.Object.new(parent);
    card.setSize(120, 80);
}
```

`defer` is Zig's RAII equivalent — guaranteed to run on scope exit, even when an error returns early.

## Comptime feature detection

The port uses `@hasDecl` to gate code on backend selection, e.g. for image-scene fallbacks:

```zig
if (@hasDecl(ove.ffi, "CONFIG_OVE_LVGL_IMG_PNG_DECODER")) {
    // Use the PNG decoder
} else {
    // Fall back to pre-decoded RGB565 assets
}
```

This collapses at compile time — only one branch ships.

## Where to next

- [Index](index.md)
- [C port](c.md) · [C++ port](cpp.md) · [Rust port](rust.md)
- [`bindings/zig/ove/`](https://github.com/Varcain/oveRTOS/tree/main/bindings/zig/ove) — Zig binding tree
