# Show a value on LVGL with thread-safe locking

**Pattern** — a worker thread produces values (sensor reading, network response, model output); a UI timer updates an LVGL widget with the latest value. LVGL is not internally locked, so all widget access goes through `ove_lvgl_lock()` / `ove_lvgl_unlock()`.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_LVGL=y
  - CONFIG_OVE_TIMER=y
  - CONFIG_OVE_THREAD=y
  - CONFIG_OVE_MUTEX=y
```

## Why two locks?

The pattern uses **two** mutexes and that surprises people:

1. **App-side mutex** (`value_mutex`) protects the shared variable between producer and UI timer.
2. **LVGL-side lock** (`ove_lvgl_lock()`) serialises every LVGL call against the LVGL graphics task that runs `lv_timer_handler()`.

Keep the app-side critical section short: copy the value out, drop the mutex, *then* acquire the LVGL lock and update the widget. Holding both at once invites priority inversion and ugly tearing.

## Code

```c
#include "ove/ove.h"
#include "ove/log.h"
#include "ove/lvgl.h"
#include <stdio.h>

OVE_LOG_MODULE_REGISTER(ui);

static ove_mutex_t  value_mutex;
static ove_timer_t  ui_timer;
static ove_thread_t producer;

static uint32_t latest_value;     /* shared, protected by value_mutex */

static lv_obj_t *value_label;
static lv_obj_t *value_bar;

/* --- Producer --- */
static void producer_fn(void *arg)
{
    (void)arg;
    uint32_t v = 0;
    while (1) {
        v++;
        ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
        latest_value = v;
        ove_mutex_unlock(value_mutex);
        ove_thread_sleep_ms(50);
    }
}

/* --- UI timer: snapshot value, then update LVGL widgets. --- */
static void ui_tick(ove_timer_t t, void *arg)
{
    (void)t; (void)arg;

    uint32_t v;
    ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
    v = latest_value;
    ove_mutex_unlock(value_mutex);     /* drop app-side lock FIRST */

    ove_lvgl_lock();
    char buf[32];
    snprintf(buf, sizeof(buf), "Count: %u", (unsigned)v);
    lv_label_set_text(value_label, buf);
    lv_bar_set_value(value_bar, (int32_t)(v % 101), LV_ANIM_ON);
    ove_lvgl_unlock();
}

static void build_ui(void)
{
    ove_lvgl_lock();
    lv_obj_t *scr = lv_screen_active();

    value_label = lv_label_create(scr);
    lv_obj_align(value_label, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(value_label, "Count: 0");

    value_bar = lv_bar_create(scr);
    lv_obj_set_size(value_bar, 240, 16);
    lv_obj_align(value_bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(value_bar, 0, 100);
    ove_lvgl_unlock();
}

void ove_main(void)
{
    ove_mutex_create(&value_mutex);
    build_ui();

    ove_thread_create(&producer, "producer", producer_fn, NULL,
                      OVE_PRIO_NORMAL, 4096);

    ove_timer_create(&ui_timer, "ui", ui_tick, NULL,
                     200 /* ms */, OVE_TIMER_PERIODIC);
    ove_timer_start(ui_timer);

    ove_run();
}
```

## Building UI from `ove_main`

The first widgets must be created before LVGL starts ticking — that's why `build_ui()` runs synchronously in `ove_main`. After that, every widget mutation must hold the LVGL lock.

If your app builds the UI from a worker thread (data-driven layout), lock the same way:

```c
ove_lvgl_lock();
lv_obj_t *card = lv_obj_create(parent);
ove_lvgl_unlock();
```

## Don't sleep with the lock held

`ove_lvgl_lock()` is a regular mutex. Anything you do under it stalls the graphics thread. So:

- No `ove_thread_sleep_ms` while locked.
- No blocking I/O (network, filesystem) while locked.
- No log statements at high frequency — `OVE_LOG_INF` is fast but not free; batch them.

If your update touches many widgets, take one lock around the whole batch rather than one per widget — fewer lock toggles, more atomic visuals.

## Binding flavors

The same pattern in the C++/Rust/Zig bindings uses RAII:

**C++**: `ove::LvglGuard guard;`

**Rust**: `let _g = ove::lvgl::lock();`

**Zig**: `var g = ove.lvgl.lock(); defer g.deinit();`

All three release on scope exit.

## Where else in the tree

- [API: Synchronization](../api/sync.md) — mutex, semaphore, condvar semantics.
- [`apps/c/heap/example/src/app.c`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example/src/app.c) — same double-lock pattern in the canonical example.
- [`apps/cpp/heap/lvgl_gallery/`](https://github.com/Varcain/oveRTOS/tree/main/apps/cpp/heap/lvgl_gallery) — comprehensive LVGL widget showcase.
