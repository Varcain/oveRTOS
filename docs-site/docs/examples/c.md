# C Example

Source: `apps/example_c/src/app.c`

The C example demonstrates the oveRTOS C API directly, using a producer-consumer pattern with optional LVGL display output. It works in both heap and zero-heap modes and runs on all supported backends.

## What the example does

Three concurrent threads communicate through a shared queue and mutex:

1. **Producer** — increments a counter every 500 ms and sends each value to a fixed-size queue.
2. **Consumer** — blocks on the queue waiting for values, then stores the latest value behind a mutex.
3. **Graphics** (optional, when `CONFIG_OVE_LVGL` is defined) — drives the LVGL display engine at ~30 fps.

A periodic software timer fires every 200 ms to read the shared counter and update the LVGL label and progress bar.

## Heap vs. zero-heap

The `_create()` / `_destroy()` API works in both heap and zero-heap modes, so the core application code is the same regardless of `CONFIG_OVE_ZERO_HEAP`:

```c
ove_queue_create(&counter_queue, sizeof(uint32_t), 8);
ove_mutex_create(&value_mutex);
ove_timer_create(&ui_timer, ui_timer_cb, NULL, 200, 0);
ove_thread_create(&prod_thread, 4096,
                  &(ove_thread_desc_t){ .entry = producer_thread,
                                        .priority = OVE_PRIO_NORMAL,
                                        .name = "producer" });
```

In heap mode, `_create()` allocates from the RTOS heap. In zero-heap mode, each `_create()` call site becomes a GCC statement-expression macro that auto-generates static storage. Size parameters (queue sizes, thread `stack_sz`) must be compile-time constants in zero-heap mode, and each call site produces one static object (do not call in a loop for multiple objects).

For **file-scope auto-initialized declarations**, the `OVE_*_DEFINE_STATIC()` macros remain available as an alternative:

```c
OVE_QUEUE_DEFINE_STATIC(counter_queue, sizeof(uint32_t), 8);
OVE_MUTEX_DEFINE_STATIC(value_mutex);
OVE_TIMER_DEFINE_STATIC(ui_timer, ui_timer_cb, NULL, 200, 0);
OVE_THREAD_DEFINE_STATIC(prod_thread, 4096, producer_thread, NULL,
                          OVE_PRIO_NORMAL, "producer");
```

For **explicit storage control** (objects in arrays, loops, or structs), use `_init()` / `_deinit()` directly.

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

`ove_queue_send` blocks for up to 1000 ms if the queue (8 slots) is full. A warning is logged and the value is dropped when the timeout expires — a safe degradation strategy for an embedded producer.

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

The consumer blocks indefinitely on `ove_queue_receive`. The mutex protects `last_value` from the UI timer callback which runs in a separate context.

## LVGL UI timer callback

```c
static void ui_timer_cb(ove_timer_t timer, void *user_data)
{
    uint32_t val;

    ove_mutex_lock(value_mutex, OVE_WAIT_FOREVER);
    val = last_value;
    ove_mutex_unlock(value_mutex);

    ove_lvgl_lock();
    snprintf(buf, sizeof(buf), "Count: %u", (unsigned int)val);
    lv_label_set_text(count_label, buf);
    lv_bar_set_value(bar, (int32_t)(val % 101), LV_ANIM_ON);
    ove_lvgl_unlock();
}
```

`ove_lvgl_lock()` / `ove_lvgl_unlock()` serialise LVGL widget access with the graphics thread. The label and progress bar cycle through values 0–100 as the counter advances.

## Entry point

```c
void ove_main(void)
{
    // Create queue, mutex, timer (heap mode)
    // Create threads
    // Initialize LVGL and build widget tree
    ove_run();  // starts the scheduler — does not return on most platforms
}
```

`ove_run()` starts the RTOS scheduler. On POSIX it blocks until all threads finish; on FreeRTOS/Zephyr/NuttX it never returns. Cleanup code after `ove_run()` is reached only on POSIX when used as a desktop test target.

## Key APIs demonstrated

| API | Purpose |
|-----|---------|
| `ove_queue_create` / `OVE_QUEUE_DEFINE_STATIC` | Create a fixed-size FIFO queue |
| `ove_queue_send` / `ove_queue_receive` | Inter-thread data transfer |
| `ove_mutex_create` / `ove_mutex_lock` / `ove_mutex_unlock` | Shared state protection |
| `ove_timer_create` / `ove_timer_start` | Periodic callback |
| `ove_thread_sleep_ms` | Rate-limiting a thread |
| `OVE_LOG_INF` / `OVE_LOG_WRN` | Compile-time filtered logging |
| `ove_lvgl_lock` / `ove_lvgl_unlock` | Safe multi-threaded LVGL access |
| `ove_run` | Start the RTOS scheduler |
