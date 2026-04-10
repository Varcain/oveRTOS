# Basic Example — C

Source: `apps/c/example/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_c/){:target="_blank"}**

The C example demonstrates the oveRTOS unified C API, using a producer-consumer pattern with optional LVGL display output. The same source code compiles unchanged in both heap and zero-heap modes across all supported backends — no `#ifdef CONFIG_OVE_ZERO_HEAP` is needed.

## Unified C API

The `_create()` / `_destroy()` API works identically in both heap and zero-heap modes:

```c
ove_queue_create(&counter_queue, sizeof(uint32_t), 8);
ove_mutex_create(&value_mutex);
ove_timer_create(&ui_timer, ui_timer_cb, NULL, 200, 0);

struct ove_thread_desc desc = {
    .name = "producer",
    .entry = producer_thread,
    .arg = NULL,
    .priority = OVE_PRIO_NORMAL,
};
ove_thread_create(&thread_handle, 4096, &desc);
```

In heap mode, `_create()` allocates from the RTOS heap. In zero-heap mode, each `_create()` call site becomes a GCC statement-expression macro that auto-generates static storage.

### Alternative allocation strategies

For **file-scope auto-initialized declarations**, the `OVE_*_DEFINE_STATIC()` macros declare a handle and initialize it before `main()`:

```c
OVE_QUEUE_DEFINE_STATIC(counter_queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(value_mutex);
OVE_THREAD_DEFINE_STATIC(prod_thread, 4096, producer_thread, NULL,
                          OVE_PRIO_NORMAL, "producer");
```

For **explicit storage control**, use `_init()` / `_deinit()` with caller-supplied storage buffers.

## Producer thread

```c
static void producer_thread(void *arg)
{
    uint32_t count = 0;
    OVE_LOG_INF("Producer started");

    while (1) {
        ++count;
        int ret = ove_queue_send(counter_queue, &count, 1000);
        if (ret != OVE_OK) {
            OVE_LOG_WRN("Producer: queue full, dropped %u", count);
        }
        ove_thread_sleep_ms(500);
    }
}
```

`ove_queue_send` blocks for up to 1000 ms if the queue (8 slots) is full. A warning is logged and the value is dropped when the timeout expires.

## Consumer thread

```c
static void consumer_thread(void *arg)
{
    uint32_t val = 0;

    while (1) {
        int ret = ove_queue_receive(counter_queue, &val, OVE_WAIT_FOREVER);
        if (ret == OVE_OK) {
            ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
            last_value = val;
            ove_mutex_unlock(value_mutex);

            if (val % 5 == 0) {
                OVE_LOG_INF("Consumer: count = %u", val);
            }
        }
    }
}
```

The consumer blocks indefinitely on `ove_queue_receive`. The mutex protects `last_value` from the UI timer callback.

## LVGL UI timer callback

```c
static void ui_timer_cb(ove_timer_t timer, void *user_data)
{
    ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
    uint32_t val = last_value;
    ove_mutex_unlock(value_mutex);

    ove_lvgl_lock();
    snprintf(buf, sizeof(buf), "Count: %u", (unsigned int)val);
    lv_label_set_text(count_label, buf);
    lv_bar_set_value(bar, (int32_t)(val % 101), LV_ANIM_ON);
    ove_lvgl_unlock();
}
```

`ove_lvgl_lock()` / `ove_lvgl_unlock()` serialise LVGL widget access with the graphics thread.

## Entry point

```c
void ove_main(void)
{
    ove_queue_create(&counter_queue, sizeof(uint32_t), 8);
    ove_mutex_create(&value_mutex);

    /* Create threads */
    struct ove_thread_desc desc = {
        .name = "producer", .entry = producer_thread,
        .priority = OVE_PRIO_NORMAL,
    };
    ove_thread_create(&thread_handle, 4096, &desc);

    /* ... consumer, graphics threads similarly ... */

    ove_lvgl_init();
    ove_lvgl_lock();
    create_ui();
    ove_lvgl_unlock();

    ove_run();  /* starts the scheduler */
}
```

## Key APIs demonstrated

| API | Purpose |
|-----|---------|
| `ove_queue_create` / `ove_queue_destroy` | Create/destroy a fixed-size FIFO queue |
| `ove_queue_send` / `ove_queue_receive` | Inter-thread data transfer |
| `ove_mutex_create` / `ove_mutex_destroy` | Create/destroy a mutex |
| `ove_mutex_lock` / `ove_mutex_unlock` | Shared state protection |
| `ove_timer_create` / `ove_timer_start` | Create and arm a periodic timer |
| `ove_thread_create` / `ove_thread_sleep_ms` | Thread lifecycle |
| `OVE_LOG_INF` / `OVE_LOG_WRN` | Compile-time filtered logging |
| `ove_lvgl_lock` / `ove_lvgl_unlock` | Safe multi-threaded LVGL access |
| `ove_run` | Start the RTOS scheduler |

## How to build

```bash
make host.posix.example_c    # or wasm.posix.example_c
make configure && make download && make && make run
```
