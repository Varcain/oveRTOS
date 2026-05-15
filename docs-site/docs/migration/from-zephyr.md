# Migrating from Zephyr

If your existing code calls Zephyr's `k_*` APIs directly (and you want to keep that code portable to FreeRTOS / NuttX), this page is the translation table.

The tables below are written against the **C surface** of oveRTOS to mirror Zephyr's C-first idiom. The same operations are available with idiomatic ergonomics through the [C++](../examples/example/cpp.md), [Rust](../examples/example/rust.md), and [Zig](../examples/example/zig.md) bindings, which is the recommended path for new code — the binding choice and the kernel choice are independent. The migration is a one-time API translation; if your existing code is Zephyr-on-C, picking a higher-level binding can happen as a follow-up step.

Two notes up front:

1. **You can run oveRTOS on the Zephyr backend** — `CONFIG_OVE_RTOS_ZEPHYR=y` keeps the Zephyr kernel underneath but presents the oveRTOS API on top. So the binary still runs on the Zephyr scheduler you know.
2. **Zephyr's threading model uses negative priorities for cooperative tasks**. oveRTOS abstracts this with named priorities (`OVE_PRIO_*`); the mapping happens inside the backend.

## Threads

| Zephyr | oveRTOS |
|---|---|
| `K_THREAD_DEFINE(id, stack, fn, p1,p2,p3, prio, opts, delay)` | `OVE_THREAD_DEFINE_STATIC(id, stack, fn, arg, prio, "id")` (one arg only — bundle as struct if you need more) |
| `k_thread_create(&th, stack, sz, fn, p1, p2, p3, prio, opts, delay)` | `ove_thread_create(&h, name, fn, arg, prio, sz)` |
| `k_thread_abort(h)` | `ove_thread_destroy(h)` |
| `k_sleep(K_MSEC(ms))` | `ove_thread_sleep_ms(ms)` |
| `k_yield()` | `ove_thread_yield()` |
| `k_current_get()` | `ove_thread_self()` |
| `k_thread_priority_get(h)` | `ove_thread_priority_get(h)` |

## Mutexes

| Zephyr | oveRTOS |
|---|---|
| `K_MUTEX_DEFINE(m)` | `OVE_MUTEX_DEFINE_STATIC(m)` |
| `k_mutex_init(&m)` | `ove_mutex_create(&m)` (heap) or implicit via the macro |
| `k_mutex_lock(&m, K_FOREVER)` | `ove_mutex_lock(m, OVE_WAIT_FOREVER)` |
| `k_mutex_unlock(&m)` | `ove_mutex_unlock(m)` |

Zephyr's mutexes are recursive by default; oveRTOS's are not. If you rely on recursion, redesign or wrap.

## Semaphores

| Zephyr | oveRTOS |
|---|---|
| `K_SEM_DEFINE(s, init, max)` | `OVE_SEMAPHORE_DEFINE_STATIC(s, init, max)` |
| `k_sem_init(&s, init, max)` | `ove_semaphore_create(&s, init, max)` |
| `k_sem_take(&s, K_MSEC(ms))` | `ove_semaphore_take(s, ms)` |
| `k_sem_give(&s)` | `ove_semaphore_give(s)` |
| `k_sem_count_get(&s)` | `ove_semaphore_count(s)` |

## Message queues

Zephyr has two queue types — `k_msgq` for fixed-size and `k_queue` / `k_fifo` for linked-list. oveRTOS exposes only the fixed-size variant (the linked-list one is less portable across the FreeRTOS / NuttX backends).

| Zephyr | oveRTOS |
|---|---|
| `K_MSGQ_DEFINE(q, item_sz, depth, align)` | `OVE_QUEUE_DEFINE_STATIC(q, item_sz, depth)` |
| `k_msgq_put(&q, &item, K_NO_WAIT)` | `ove_queue_send(q, &item, 0)` |
| `k_msgq_get(&q, &item, K_FOREVER)` | `ove_queue_receive(q, &item, OVE_WAIT_FOREVER)` |
| `k_msgq_num_used_get(&q)` | `ove_queue_count(q)` |
| `k_msgq_purge(&q)` | `ove_queue_reset(q)` |

For `k_fifo` / `k_lifo` patterns, port to a queue of pointer-sized items.

## Timers

| Zephyr | oveRTOS |
|---|---|
| `K_TIMER_DEFINE(t, expiry, stop)` | `OVE_TIMER_DEFINE_STATIC(t, "t", expiry, arg, period_ms, OVE_TIMER_PERIODIC)` |
| `k_timer_start(&t, K_MSEC(d), K_MSEC(p))` | `ove_timer_start(t)` after `ove_timer_set_period(t, p)` for periodic; oneshot uses the `OVE_TIMER_ONESHOT` flag and `ove_timer_set_period(t, d)` |
| `k_timer_stop(&t)` | `ove_timer_stop(t)` |
| `k_timer_status_get(&t)` | `ove_timer_is_running(t)` (boolean) |

The `expiry_fn(struct k_timer *)` signature becomes `void cb(ove_timer_t t, void *user_data)`.

## Events / event flags

Zephyr's `k_event` maps onto `ove_eventgroup`:

| Zephyr | oveRTOS |
|---|---|
| `K_EVENT_DEFINE(e)` | `OVE_EVENTGROUP_DEFINE_STATIC(e)` |
| `k_event_post(&e, mask)` | `ove_eventgroup_set(e, mask)` |
| `k_event_set(&e, mask)` | `ove_eventgroup_set_overwrite(e, mask)` |
| `k_event_clear(&e, mask)` | `ove_eventgroup_clear(e, mask)` |
| `k_event_wait(&e, mask, reset, T)` | `ove_eventgroup_wait(e, mask, &got, flags, T_ms)` |

## Stack-based memory (slabs, pools)

Zephyr's `k_mem_slab` / `k_heap` / `k_mem_pool` have no portable equivalent. For your port:

- Fixed-size object pools → static array + a free-list, or a queue of pointers.
- `k_heap_alloc` → `ove_malloc` in heap mode; in zero-heap mode, redesign to use compile-time storage.

This is the part of a Zephyr port that takes the most thought.

## Workqueues

Zephyr's system workqueue maps closely:

| Zephyr | oveRTOS |
|---|---|
| `k_work_init(&w, handler)` | `ove_work_init(&w, handler, arg)` |
| `k_work_submit(&w)` | `ove_work_submit(&w)` (defaults to system workqueue) |
| `k_work_submit_to_queue(&q, &w)` | `ove_work_submit_to(q, &w)` |
| `K_WORK_DEFINE(w, handler)` | `OVE_WORK_DEFINE_STATIC(w, handler, arg)` |

Delayable work (`k_work_delayable`) is not a separate type in oveRTOS — schedule a one-shot timer that submits the work.

## Devices and drivers

Zephyr's `DEVICE_DT_GET(...)` / device-tree node IDs do **not** carry over. Each oveRTOS module has its own typed handle:

| Zephyr | oveRTOS |
|---|---|
| `const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(usart1));` | `ove_uart_t uart; ove_uart_open(&uart, OVE_UART_DEV_0, &cfg);` |
| `uart_poll_in(uart, &c)` | `ove_uart_read(uart, &c, 1, 0)` |
| `gpio_pin_configure_dt(&spec, GPIO_INPUT)` | `ove_gpio_open(&pin, OVE_GPIO_PIN(port, bit), &cfg)` |
| `i2c_write_dt(&spec, buf, len)` | `ove_i2c_write(i2c, addr, buf, len)` |

The board's `board.yaml` (oveRTOS-side) plays a role similar to the device tree: it declares which peripherals exist and how they're wired, which the BSP turns into the right `OVE_*_DEV_*` constants.

## Networking

Zephyr ships its own networking stack; oveRTOS uses lwIP under FreeRTOS, NuttX's networking under NuttX, and Zephyr's stack under Zephyr — but the **API is the same**:

| Zephyr | oveRTOS |
|---|---|
| `zsock_socket(AF_INET, SOCK_STREAM, 0)` | `ove_socket(AF_INET, SOCK_STREAM, 0)` |
| `zsock_connect(s, &addr, len)` | `ove_connect(s, &addr, len)` |
| `zsock_send(s, buf, len, 0)` | `ove_send(s, buf, len, 0)` |
| `tls_credential_add(...)` | `ove_tls_set_trust_store(pem, len)` |

So the migration is "drop the `z`" for most socket calls.

## Logging

| Zephyr | oveRTOS |
|---|---|
| `LOG_MODULE_REGISTER(name, LOG_LEVEL_INF)` | `OVE_LOG_MODULE_REGISTER(name)` (level from Kconfig) |
| `LOG_INF("...", ...)` | `OVE_LOG_INF("...", ...)` |
| `LOG_HEXDUMP_DBG(buf, len, "tag")` | `OVE_LOG_HEXDUMP_DBG("tag", buf, len)` |

## See also

- [Architecture Overview](../getting-started/overview.md)
- [API Reference](../api/index.md)
- [Backends → Zephyr](../backends/index.md) — what oveRTOS does and doesn't expose from Zephyr internals
