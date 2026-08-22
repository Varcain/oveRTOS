# Migrating from Zephyr

If your existing code calls Zephyr's `k_*` APIs directly (and you want to keep that code portable to FreeRTOS / NuttX), this page is the translation table.

The tables below are written against the **C surface** of oveRTOS to mirror Zephyr's C-first idiom. The same operations are available with idiomatic ergonomics through the [C++](../examples/example/cpp.md), [Rust](../examples/example/rust.md), and [Zig](../examples/example/zig.md) bindings, which is the recommended path for new code — the binding choice and the kernel choice are independent.

Three notes up front:

1. **You can run oveRTOS on the Zephyr backend** — `CONFIG_OVE_RTOS_ZEPHYR=y` keeps the Zephyr kernel underneath but presents the oveRTOS API on top. So the binary still runs on the Zephyr scheduler you know.
2. **Timeouts are nanoseconds**, not `k_timeout_t`. Use the `OVE_MS(n)` / `OVE_SEC(n)` helpers from `ove/types.h`, or `OVE_WAIT_FOREVER` for an unbounded wait. Plain millisecond `uint32_t`s appear only on duration-style APIs (`ove_thread_sleep_ms`, `ove_timer_create` period).
3. **Priorities are named, not numeric.** Zephyr uses negative priorities for cooperative tasks; oveRTOS abstracts this with named priorities (`OVE_PRIO_IDLE`, `OVE_PRIO_LOW`, `OVE_PRIO_NORMAL`, `OVE_PRIO_HIGH`, `OVE_PRIO_CRITICAL`). The mapping happens inside the backend.

## Threads

| Zephyr | oveRTOS |
|---|---|
| `K_THREAD_DEFINE(id, stack, fn, p1,p2,p3, prio, opts, delay)` | `OVE_THREAD_DEFINE_STATIC(id, stack, fn, arg, prio, "id")` (one arg only — bundle as struct if you need more) |
| `k_thread_create(&th, stack, sz, fn, p1, p2, p3, prio, opts, delay)` | `ove_thread_create(&h, name, fn, arg, prio, sz)` |
| `k_thread_abort(h)` | `ove_thread_destroy(h)` |
| `k_sleep(K_MSEC(ms))` | `ove_thread_sleep_ms(ms)` |
| `k_yield()` | `ove_thread_yield()` |
| `k_current_get()` | `ove_thread_get_self()` |
| `k_thread_priority_set(h, p)` | `ove_thread_set_priority(h, p)` |

There is no portable getter for priority — keep your own copy if you need to read it back.

## Mutexes

| Zephyr | oveRTOS |
|---|---|
| `K_MUTEX_DEFINE(m)` | `OVE_MUTEX_DEFINE_STATIC(m)` |
| `k_mutex_init(&m)` | `ove_mutex_create(&m)` (heap) or implicit via the macro |
| `k_mutex_lock(&m, K_FOREVER)` | `ove_mutex_lock(m, OVE_WAIT_FOREVER)` |
| `k_mutex_lock(&m, K_MSEC(ms))` | `ove_mutex_lock(m, OVE_MS(ms))` |
| `k_mutex_unlock(&m)` | `ove_mutex_unlock(m)` |

Zephyr's `k_mutex` is recursive by default; the plain `ove_mutex_*` is **not**. Use `ove_recursive_mutex_*` if you rely on recursion.

## Semaphores

| Zephyr | oveRTOS |
|---|---|
| `K_SEM_DEFINE(s, init, max)` | `OVE_SEM_DEFINE_STATIC(s, init, max)` |
| `k_sem_init(&s, init, max)` | `ove_sem_create(&s, init, max)` |
| `k_sem_take(&s, K_MSEC(ms))` | `ove_sem_take(s, OVE_MS(ms))` |
| `k_sem_take(&s, K_FOREVER)` | `ove_sem_take(s, OVE_WAIT_FOREVER)` |
| `k_sem_give(&s)` | `ove_sem_give(s)` |

`ove_sem_give` has no ISR-safe variant — use `ove_event_signal_from_isr` (single-bit) or `ove_eventgroup_set_bits_from_isr` (bitmask) for ISR→thread signalling. There is no portable `k_sem_count_get` analogue.

## Message queues

Zephyr has two queue types — `k_msgq` for fixed-size and `k_queue` / `k_fifo` for linked-list. oveRTOS exposes only the fixed-size variant (the linked-list one is less portable across the FreeRTOS / NuttX backends).

| Zephyr | oveRTOS |
|---|---|
| `K_MSGQ_DEFINE(q, item_sz, depth, align)` | `OVE_QUEUE_DEFINE_STATIC(q, item_sz, depth)` |
| `k_msgq_put(&q, &item, K_NO_WAIT)` | `ove_queue_send(q, &item, 0)` |
| `k_msgq_put(&q, &item, K_MSEC(ms))` | `ove_queue_send(q, &item, OVE_MS(ms))` |
| `k_msgq_get(&q, &item, K_FOREVER)` | `ove_queue_receive(q, &item, OVE_WAIT_FOREVER)` |

For `k_fifo` / `k_lifo` patterns, port to a queue of pointer-sized items.

## Timers

`ove_timer_create()` takes the timer object directly — no name, no expiry/stop callback pair (only an expiry callback). The "one-shot" toggle is a plain `int` (non-zero → one-shot, zero → periodic). Period is in **milliseconds**.

| Zephyr | oveRTOS |
|---|---|
| `K_TIMER_DEFINE(t, expiry, stop)` | `OVE_TIMER_DEFINE_STATIC(t, expiry, user_data, period_ms, 0 /* periodic */)` |
| `k_timer_init(&t, expiry, stop)` | `ove_timer_create(&t, expiry, user_data, period_ms, 0)` |
| `k_timer_start(&t, K_MSEC(d), K_MSEC(p))` | `ove_timer_start(t)` — start delay is not separately exposed; the timer fires after the first period elapses |
| `k_timer_stop(&t)` | `ove_timer_stop(t)` |

The `expiry_fn(struct k_timer *)` signature becomes `void cb(ove_timer_t t, void *user_data)`.

## Events / event bits

Zephyr's `k_event` maps onto `ove_eventgroup`. The naming uses the `_bits` suffix and timeouts are nanoseconds:

| Zephyr | oveRTOS |
|---|---|
| `K_EVENT_DEFINE(e)` | `OVE_EVENTGROUP_DEFINE_STATIC(e)` |
| `k_event_post(&e, mask)` | `ove_eventgroup_set_bits(e, mask)` |
| `k_event_clear(&e, mask)` | `ove_eventgroup_clear_bits(e, mask)` |
| `k_event_wait(&e, mask, reset, T)` | `ove_eventgroup_wait_bits(e, mask, flags, OVE_MS(ms), &got)` |
| `k_event_test(&e, mask)` | `(ove_eventgroup_get_bits(e) & mask)` |

`flags` is `OVE_EVENT_WAIT_ALL` or `OVE_EVENT_WAIT_ANY`, optionally `| OVE_EVENT_CLEAR_ON_EXIT`. There is no atomic "overwrite" variant; use `_clear_bits` followed by `_set_bits` if you need set-with-reset semantics.

## Workqueues

Each work item is submitted to a specific workqueue; oveRTOS does not expose a global "system workqueue". Create your own with `ove_workqueue_create` (heap) or `OVE_WORKQUEUE_DEFINE_STATIC`.

| Zephyr | oveRTOS |
|---|---|
| `K_WORK_DEFINE(w, handler)` | `OVE_WORK_DEFINE_STATIC(w, handler)` |
| `k_work_init(&w, handler)` | `ove_work_init(&w, handler)` (heap) or `ove_work_init_static(&w, &storage, handler)` |
| `k_work_submit_to_queue(&q, &w)` | `ove_work_submit(wq, w)` |
| `k_work_submit(&w)` (system queue) | not exposed — submit to a workqueue you've created |

Delayable work (`k_work_delayable`) is not a separate type — use `ove_work_submit_delayed(wq, w, delay_ms)`.

## Stack-based memory (slabs, pools)

Zephyr's `k_mem_slab` / `k_heap` / `k_mem_pool` have no portable equivalent. For your port:

- Fixed-size object pools → static array + a free-list, or a queue of pointers.
- `k_heap_alloc` → use heap mode and rely on `_create()` constructors; in zero-heap mode, redesign to use caller-owned storage.

This is the part of a Zephyr port that takes the most thought.

## Devices and drivers

Zephyr's `DEVICE_DT_GET(...)` / device-tree node IDs do **not** carry over. Each oveRTOS module has its own typed handle and constructor:

| Zephyr | oveRTOS |
|---|---|
| `const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(usart1));` | `ove_uart_t uart; ove_uart_create(&uart, &cfg);` (heap) or `ove_uart_init(&uart, &storage, …)` |
| `uart_poll_in(uart, &c)` | `ove_uart_read(uart, &c, 1, 0, NULL)` |
| `gpio_pin_configure_dt(&spec, GPIO_INPUT)` | `ove_gpio_configure(port, pin, &cfg)` |
| `i2c_write_dt(&spec, buf, len)` | `ove_i2c_write(i2c, addr, buf, len, OVE_WAIT_FOREVER)` |

The board's `board.yaml` (oveRTOS-side) plays a role similar to the device tree: it declares which peripherals exist and how they're wired, which the BSP turns into the right `OVE_*_DEV_*` constants.

## Networking

Zephyr ships its own networking stack; oveRTOS uses lwIP under FreeRTOS, NuttX's networking under NuttX, and Zephyr's stack under Zephyr — but the **API is the same** across all three:

| Zephyr | oveRTOS |
|---|---|
| `zsock_socket(AF_INET, SOCK_STREAM, 0)` | `ove_socket_create(&sock, OVE_AF_INET, OVE_SOCK_STREAM)` (heap) or `ove_socket_open(&sock, &storage, …)` |
| `zsock_connect(s, &addr, len)` | `ove_socket_connect(sock, &addr, OVE_WAIT_FOREVER)` |
| `zsock_send(s, buf, len, 0)` | `ove_socket_send(sock, buf, len, &sent)` |
| `zsock_recv(s, buf, len, 0)` | `ove_socket_recv(sock, buf, len, &received, OVE_WAIT_FOREVER)` |
| `tls_credential_add(...)` | populate `ove_tls_config_t` and pass to `ove_tls_handshake(tls, sock, &cfg)` |

## Logging

| Zephyr | oveRTOS |
|---|---|
| `LOG_MODULE_REGISTER(name, LOG_LEVEL_INF)` | No registration needed; level comes from Kconfig |
| `LOG_ERR("...", ...)` | `OVE_LOG_ERR("...", ...)` |
| `LOG_WRN("...", ...)` | `OVE_LOG_WRN("...", ...)` |
| `LOG_INF("...", ...)` | `OVE_LOG_INF("...", ...)` |
| `LOG_DBG("...", ...)` | `OVE_LOG_DBG("...", ...)` |

Hex-dump and instance-level logging are not currently part of the portable surface.

## See also

- [Architecture Overview](../getting-started/overview.md)
- [API Reference](../api/index.md)
- [Backends → Zephyr](../backends/index.md) — what oveRTOS does and doesn't expose from Zephyr internals
