# LVGL Benchmark — C

Source: `apps/c/heap/lvgl_benchmark/src/app.c` (and `apps/c/zeroheap/lvgl_benchmark/`).

The C port calls the LVGL C API directly through `<ove/lvgl.h>` — `ove_lvgl_lock()` / `ove_lvgl_unlock()` are the only oveRTOS-specific calls; everything else is upstream LVGL.

## Build

```bash
make host.posix.lvgl_benchmark      # POSIX host + browser dashboard
make qemu.freertos.lvgl_benchmark   # QEMU + FreeRTOS
make                                 # build
make run                             # run
```

## Scene registration

Each scene is a function that builds a tree of LVGL widgets on `lv_screen_active()`:

```c
static void rectangle_create(void)
{
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    lv_obj_set_size(obj, 200, 200);
    lv_obj_center(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xff0000), 0);
}
```

A static table pairs the scene name with its builder:

```c
static scene_dsc_t scenes[] = {
    { "Empty screen",            empty_create,             0, 0, 0, 0, 0, 0 },
    { "Rectangle",               rectangle_create,         0, 0, 0, 0, 0, 0 },
    { "Rectangle, rounded",      rectangle_rounded_create, 0, 0, 0, 0, 0, 0 },
    /* … */
};
```

## Scene rotation

A periodic LVGL timer fires every `scene_time` ms to record metrics and load the next scene:

```c
static void next_scene_timer_cb(lv_timer_t *timer)
{
    /* Record current scene's metrics from LVGL's perf monitor. */
    scenes[current_scene].fps_avg = ...;

    current_scene++;
    if (current_scene >= ARRAY_SIZE(scenes)) {
        summary_create();
        lv_timer_del(timer);
        return;
    }

    lv_obj_clean(lv_screen_active());
    scenes[current_scene].create_cb();
}
```

`lv_timer_*` is LVGL's own timer engine, not `ove_timer_*` — LVGL timers run from `lv_timer_handler()` on the graphics thread, so they're already synchronised with the rest of LVGL.

## Locking

The benchmark builds and tears down widgets from the graphics thread, so it doesn't need explicit `ove_lvgl_lock()` calls. If your own benchmark variation builds widgets from another thread (a network response, a sensor sample), wrap the construction:

```c
ove_lvgl_lock();
lv_obj_t *card = lv_obj_create(parent);
ove_lvgl_unlock();
```

## Assets

Image scenes use LVGL's compile-time image format. The C array is generated at build time from PNGs and listed in `app.yaml`:

```yaml
sources:
  - src/app.c
  - src/benchmark_perf.c
  - src/assets/benchmark_cogwheel_rgb.c
  - src/assets/benchmark_cogwheel_argb.c
  - src/assets/benchmark_avatar.c
```

The PNG-to-C conversion happens via `scripts/lvgl_img_conv.py` invoked by the build system.

## Where to next

- The summary table at the end of the run pastes cleanly into `make benchmarks` reports.
- Compare numbers across bindings: [C++](cpp.md), [Rust](rust.md), [Zig](zig.md).
- LVGL API: <https://docs.lvgl.io/master/>
- oveRTOS LVGL integration internals: `bindings/cpp/ove/lvgl.hpp` (the highest-level wrapper still pairs to the raw C API used here).
