# C++ Example

Source: `apps/cpp/example/src/app.cpp`

The C++ example demonstrates the `ove::lvgl` C++20 wrapper layer. It implements the same producer-consumer pattern as the C example, but replaces manual handles and C-style API calls with RAII types, fluent widget builders, and reactive state bindings.

## Includes and namespaces

```cpp
#include <ove/ove.hpp>      // C++20 oveRTOS wrapper
#include <ove/lvgl.hpp>     // LVGL C++ wrapper (when CONFIG_OVE_LVGL)

namespace lv = ove::lvgl;
```

`ove/ove.hpp` re-exports the C API under C++ wrappers. `ove/lvgl.hpp` provides the fluent builder types used to construct the UI.

## Static object declaration

All RTOS objects are declared at file scope with no heap allocation — no explicit init call is needed:

```cpp
static ove::Queue<uint32_t, 8> counter_queue;
static ove::Mutex value_mutex;
static ove::Timer ui_timer(ui_timer_cb, nullptr, 200);
static ove::Thread<4096> prod_thread(producer_thread, nullptr,
                                      OVE_PRIO_NORMAL, "producer");
static ove::Thread<4096> cons_thread(consumer_thread, nullptr,
                                      OVE_PRIO_NORMAL, "consumer");
```

The template parameters encode configuration at compile time (`Thread<4096>` fixes the stack size; `Queue<uint32_t, 8>` fixes the item type and depth), preventing mismatched runtime parameters.

## RAII locking with LockGuard

The consumer thread uses `ove::LockGuard` for automatic mutex release:

```cpp
static void consumer_thread(void *arg)
{
    while (true) {
        int ret = counter_queue.receive(&val, OVE_WAIT_FOREVER);
        if (ret == OVE_OK) {
            {
                ove::LockGuard lock(value_mutex);  // unlocks on scope exit
                last_value = val;
            }
            if (val % 5 == 0) {
                OVE_LOG_INF("Consumer: count = %u", val);
            }
        }
    }
}
```

The inner block scope ensures `value_mutex` is released before the log call. `ove::LockGuard` follows the standard C++ RAII pattern: the mutex is locked in the constructor and released in the destructor.

## CounterComponent — CRTP component composition

The UI is structured as a `Component<T>` using the CRTP pattern:

```cpp
class CounterComponent : public lv::Component<CounterComponent> {
public:
    lv::State<int> m_count{0};   // reactive state (when LV_USE_OBSERVER)
    lv::Bar m_bar{nullptr};

    lv::ObjectView build(lv::ObjectView parent) {
        auto root = lv::Box::create(parent)
            .size(LV_PCT(100), LV_PCT(100))
            .bg_color(lv_color_black())
            .bg_opa(LV_OPA_COVER);

        lv::Label::create(root)
            .text(APP_TITLE)
            .font(&lv_font_montserrat_32)
            .color(lv_color_white())
            .align(LV_ALIGN_TOP_MID, 0, 16);

        lv::Label::create(root)
            .bind_text(m_count, "Count: %d")   // reactive binding
            .font(&lv_font_montserrat_14)
            .color(lv_color_white())
            .align(LV_ALIGN_TOP_MID, 0, 64);

        m_bar = lv::Bar::create(root)
            .size(200, 16)
            .range(0, 100)
            .indicator_color(lv_palette_main(LV_PALETTE_BLUE))
            .radius(8)
            .align(LV_ALIGN_TOP_MID, 0, 96);

        return root;
    }

    void update(uint32_t val) {
        m_count.set(static_cast<int32_t>(val));  // notifies bound labels
        if (m_bar) m_bar.value(val % 101, LV_ANIM_ON);
    }
};
```

`build()` constructs the widget tree using fluent method chaining. `.bind_text(m_count, "Count: %d")` creates a reactive binding: whenever `m_count.set()` is called, LVGL's observer framework automatically updates the label text without a timer callback or manual string formatting (requires `LV_USE_OBSERVER`). When LVGL observer support is unavailable, the example falls back to a manual `text_fmt` call in `update()`.

## LVGL guard in the UI timer callback

```cpp
static void ui_timer_cb(ove_timer_t, void *)
{
    uint32_t val;
    {
        ove::LockGuard lock(value_mutex);
        val = last_value;
    }

    lv::LvglGuard guard;          // locks LVGL for the duration of this scope
    counter_component.update(val);
}
```

`lv::LvglGuard` wraps `ove_lvgl_lock()` / `ove_lvgl_unlock()` in an RAII guard, ensuring the LVGL lock is always released even if `update()` throws.

## Entry point

```cpp
OVE_MAIN()
{
    OVE_LOG_INF("C++ example: init");

    int ret = ove_lvgl_init();
    {
        lv::LvglGuard guard;
        counter_component.mount(lv::ObjectView::screen_active());
    }

    ui_timer.start();
    ove::run();  // start scheduler

    // Cleanup (reached only on POSIX)
    {
        lv::LvglGuard guard;
        counter_component.unmount();
    }
}
```

`OVE_MAIN()` expands to `void ove_main()`. `counter_component.mount()` calls the `build()` method and attaches the resulting widget tree to the active screen. `counter_component.unmount()` removes the widgets on shutdown.

## Key APIs demonstrated

| API | Purpose |
|-----|---------|
| `ove::Queue<T, N>` | Type-safe, fixed-depth message queue |
| `ove::Mutex` / `ove::LockGuard` | RAII mutex ownership |
| `ove::Thread<StackSize>` | Compile-time stack-sized thread |
| `ove::Timer` | Software timer with C++ callback |
| `lv::Component<T>` | CRTP widget composition base |
| `lv::State<T>` | Reactive observable value |
| `lv::Label::bind_text` | Reactive label binding |
| `lv::LvglGuard` | RAII LVGL display lock |
| `lv::Box` / `lv::Label` / `lv::Bar` | Fluent widget builders |
| `ove::run()` | Start the RTOS scheduler |

---

## Networking Example

Source: `apps/cpp/example_net/`

The C++ networking example uses RAII wrappers for all network resources with a structured test harness (TEST/PASS/FAIL tracking with results summary). DNS (positive and negative), TCP, UDP echo, HTTP GET/POST/PUT with custom headers, SNTP time sync, MQTT pub/sub, and the built-in HTTPD dashboard are all tested using `ove::NetIf`, `ove::TcpSocket`, `ove::UdpSocket`, `ove::dns`, `ove::http`, `ove::sntp`, `ove::mqtt`, and `ove::httpd` namespaces. Resources clean up automatically via destructors. Entry point uses `OVE_MAIN()` and `ove::run()`. See the [Networking API Guide](../api/net.md) for the full API.
