# Basic Example — C++

Source: `apps/cpp/example/src/app.cpp` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_cpp/){:target="_blank"}**

The C++ example demonstrates the `ove::` namespace wrappers with RAII semantics, template-based objects, and the LVGL C++20 component system.

## C++ RAII patterns

Objects are declared at file scope — no `_create()` / `_destroy()` calls needed:

```cpp
static ove::Queue<uint32_t, 8> counter_queue;
static ove::Mutex value_mutex;
static ove::Thread<4096> prod_thread(producer_thread, nullptr,
                                      OVE_PRIO_NORMAL, "producer");
static ove::Thread<4096> cons_thread(consumer_thread, nullptr,
                                      OVE_PRIO_NORMAL, "consumer");
```

`ove::Queue<T, N>` is a fixed-size typed queue. `ove::Thread<StackSize>` auto-allocates its stack at compile time. `ove::Mutex` wraps `ove_mutex_t` with RAII.

## Producer thread

```cpp
static void producer_thread(void *)
{
    uint32_t count = 0;
    OVE_LOG_INF("Producer started");

    while (true) {
        ++count;
        if (counter_queue.send(count, 1000) != ove::Error::Ok) {
            OVE_LOG_WRN("Producer: queue full, dropped %u", count);
        }
        ove::Thread<>::sleep_ms(500);
    }
}
```

`counter_queue.send()` returns `ove::Error` instead of a raw int. `ove::Thread<>::sleep_ms()` is a static method on the thread class.

## LVGL component composition

The C++ example uses the LVGL C++20 wrapper with fluent builders:

```cpp
namespace lv = ove::lvgl;

lv::LvglGuard guard;
auto *screen = lv::screen_active();
screen->set_style_bg_color(lv::Color::black(), 0);

title_label = lv::Label::create(screen)
    .set_text(APP_TITLE)
    .set_style_text_font(&lv_font_montserrat_32, 0)
    .align(LV_ALIGN_TOP_MID, 0, 16);
```

`lv::LvglGuard` is an RAII lock — it calls `ove_lvgl_lock()` on construction and `ove_lvgl_unlock()` on destruction.

## Entry point

```cpp
OVE_MAIN()
{
    OVE_LOG_INF("C++ example: init");

    /* Objects are auto-created at file scope via constructors.
     * Just init LVGL and start the scheduler. */
    ove::lvgl::init();
    { lv::LvglGuard g; create_ui(); }
    ui_timer.start();

    ove::run();
}
```

No manual `_create()` / `_destroy()` — C++ constructors and destructors handle lifecycle.

## Key APIs demonstrated

| C++ API | C Equivalent | Purpose |
|---------|-------------|---------|
| `ove::Queue<T, N>` | `ove_queue_create` | Typed fixed-size queue |
| `ove::Mutex` | `ove_mutex_create` | RAII mutex |
| `ove::Thread<N>` | `ove_thread_create` | Thread with compile-time stack |
| `ove::Timer` | `ove_timer_create` | Periodic callback timer |
| `lv::LvglGuard` | `ove_lvgl_lock/unlock` | RAII LVGL locking |
| `lv::Label::create()` | `lv_label_create` | Fluent widget builder |
| `ove::run()` | `ove_run` | Start scheduler |

## How to build

```bash
make host.posix.example_cpp    # or wasm.posix.example_cpp
make configure && make download && make && make run
```
