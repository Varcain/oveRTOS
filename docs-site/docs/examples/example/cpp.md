# Basic Example — C++

Source: `apps/cpp/example/src/app.cpp` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_cpp_heap/){:target="_blank"}**

The C++ example demonstrates the `ove::` namespace wrappers with RAII semantics, template-based objects, `Result<T>` error handling, and the LVGL C++20 fluent builders.

The C++ binding builds with **`-std=c++23`** across all backends (POSIX, FreeRTOS, NuttX, Zephyr).

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

`ove::Queue<T, N>` is a fixed-size typed queue (compile-time enforced `std::is_trivially_copyable_v<T>`). `ove::Thread<StackSize>` auto-allocates its stack at compile time. `ove::Mutex` wraps `ove_mutex_t` with RAII.

## `Result<T>` and `Error`

Fallible operations across the binding return `ove::Result<T>` — an alias for `std::expected<T, ove::Error>` — instead of raw `int` rc-codes. The error side is a typed `enum class ove::Error` mirroring every substrate `OVE_ERR_*` constant. Both can flow through `std::error_code` via the bundled category.

```cpp
// Forever-blocking forms assert on failure (programming error):
mtx.lock();                    // no return; aborts on substrate failure
queue.send(item);              // forever-wait variant
queue.receive(out);

// Bounded forms return Result<void>:
if (auto r = mtx.try_lock_for(100ms); r) {
    /* got the lock */
} else if (r.error() == ove::Error::Timeout) {
    /* deadline elapsed */
}

// Methods that produce a value return Result<T>:
auto r = sock.send(buf, len);          // Result<size_t>
if (r && *r == len) { /* fully sent */ }

auto resp = http_client.get(url);      // Result<Response>
if (resp) { use(resp->status(), resp->body()); }
```

## Producer thread

```cpp
static void producer_thread(void *)
{
    uint32_t count = 0;
    OVE_LOG_INF("Producer started");

    using namespace std::chrono_literals;

    while (true) {
        ++count;
        if (!counter_queue.try_send_for(count, 1000ms)) {
            OVE_LOG_WRN("Producer: queue full, dropped %u", count);
        }
        ove::this_thread::sleep_ms(500);
    }
}
```

`try_send_for(item, duration)` returns `Result<void>` — `operator bool` is `has_value()`, so `if (!result)` reads as "send didn't succeed." Use `result.error()` to discriminate `Error::Timeout` vs `Error::QueueFull` if needed.

`ove::this_thread::sleep_ms()` mirrors `std::this_thread::sleep_for` shape (the namespace also exposes `sleep_for(duration)`, `yield()`, `get_id()`, `self()`).

## Composing with the standard library

`ove::Mutex` satisfies the `BasicLockable` and `Lockable` named requirements, so it composes directly with `std::lock_guard` and `std::scoped_lock`:

```cpp
ove::Mutex a, b;

{
    std::lock_guard<ove::Mutex> g(a);
    /* a is held */
}

{
    std::scoped_lock g(a, b);  // deadlock-avoidance dance
    /* both held */
}
```

`stop_token` / `stop_source` mirror `std::stop_token` / `std::stop_source` shape for cooperative thread cancellation; capturing-lambda constructors mirror `std::jthread`.

## LVGL UI

The C++ example uses the LVGL C++20 wrapper with fluent builders:

```cpp
namespace lv = ove::lvgl;

lv::LvglGuard guard;
auto scr = lv::ObjectView::screen_active();
lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

lv::Label::create(scr)
    .text(APP_TITLE)
    .font(&lv_font_montserrat_32)
    .color(lv_color_white())
    .align(LV_ALIGN_TOP_MID, 0, 16);
```

`lv::LvglGuard` is an RAII lock — it calls `ove_lvgl_lock()` on construction and `ove_lvgl_unlock()` on destruction. The fluent setters drop the LVGL `set_` prefix (`text`, `font`, `color`, `radius`, …) for terser chains.

## Entry point

```cpp
OVE_MAIN()
{
    OVE_LOG_INF("C++ example: init");

    /* Objects are auto-created at file scope via constructors.
     * Just init LVGL and start the scheduler. */
    ove::lvgl::init();
    { lv::LvglGuard g; create_ui(); }

    if (auto r = ui_timer.start(); !r) {
        OVE_LOG_ERR("UI timer start failed: %d", static_cast<int>(r.error()));
        return;
    }

    ove::run();
}
```

No manual `_create()` / `_destroy()` — C++ constructors and destructors handle lifecycle.

## Key APIs demonstrated

| C++ API | C Equivalent | Purpose |
|---------|-------------|---------|
| `ove::Queue<T, N>` | `ove_queue_create` | Typed fixed-size queue, `Result<void>` returns |
| `ove::Mutex` | `ove_mutex_create` | RAII mutex, satisfies `std::Lockable` |
| `ove::Thread<N>` | `ove_thread_create` | Thread with compile-time stack, `jthread`-style cancel |
| `ove::Timer` | `ove_timer_create` | Periodic callback timer |
| `ove::this_thread::*` | `ove_thread_yield/sleep_ms` | `std::this_thread`-shaped helpers |
| `ove::Result<T>` | — | `std::expected<T, ove::Error>` for fallible ops |
| `lv::LvglGuard` | `ove_lvgl_lock/unlock` | RAII LVGL locking |
| `lv::Label::create()` | `lv_label_create` | Fluent widget builder |
| `ove::run()` | `ove_run` | Start scheduler |

## How to build

```bash
make host.posix.example_cpp_heap   # or wasm.posix.example_cpp_heap
make configure && make download && make && make run
```
