# Migrating from raw FreeRTOS

If you're coming from an existing FreeRTOS application (one that calls `xTaskCreate`, `xQueueSend`, etc. directly), this page is the translation table from FreeRTOS-native APIs to the oveRTOS application API.

The tables below are written against the **C surface** because that's the closest analogue to FreeRTOS. The same operations are also available — with idiomatic ergonomics — through the [C++](../examples/example/cpp.md), [Rust](../examples/example/rust.md), and [Zig](../examples/example/zig.md) bindings, and that is the recommended path for new code. Picking a higher-level binding is a per-project decision; the kernel choice (FreeRTOS, Zephyr, NuttX) is independent.

Most concepts map one-to-one. The two real differences:

1. **Allocation model** — FreeRTOS has heap-and-static at the API surface (`xTaskCreate` vs `xTaskCreateStatic`); oveRTOS has heap-and-static at the *build configuration* level (`_create()` vs `OVE_*_DEFINE_STATIC()` driven by `CONFIG_OVE_ZERO_HEAP`).
2. **Error reporting** — FreeRTOS returns `pdPASS` / `pdFAIL` / typed values; oveRTOS returns a uniform `OVE_OK` / negative-on-error pattern across every module. The C++ binding wraps this in exceptions-as-values; Rust returns `Result<T, Error>`; Zig returns `Error!T`.

## Threads (tasks)

| FreeRTOS | oveRTOS (heap mode) | oveRTOS (zero-heap mode) |
|---|---|---|
| `xTaskCreate(fn, name, stack, arg, prio, &h)` | `ove_thread_create(&h, name, fn, arg, prio, stack)` | `OVE_THREAD_DEFINE_STATIC(h, stack, fn, arg, prio, name)` |
| `xTaskCreateStatic(fn, name, stack, arg, prio, stackbuf, &tcb)` | n/a — use the `OVE_THREAD_DEFINE_STATIC` macro | same macro |
| `vTaskDelete(h)` | `ove_thread_destroy(h)` | n/a — static lifetime |
| `vTaskDelay(pdMS_TO_TICKS(ms))` | `ove_thread_sleep_ms(ms)` | same |
| `taskYIELD()` | `ove_thread_yield()` | same |
| `uxTaskPriorityGet(h)` | `ove_thread_priority_get(h)` | same |

Priority levels map: `tskIDLE_PRIORITY` → `OVE_PRIO_IDLE`, then a small set of named constants (`OVE_PRIO_LOW`, `OVE_PRIO_NORMAL`, `OVE_PRIO_HIGH`, `OVE_PRIO_CRITICAL`). The portable API uses names because FreeRTOS, Zephyr, and NuttX disagree on numeric direction (lower = higher in FreeRTOS, opposite in Zephyr).

## Mutexes

| FreeRTOS | oveRTOS |
|---|---|
| `xSemaphoreCreateMutex()` | `ove_mutex_create(&m)` |
| `xSemaphoreCreateRecursiveMutex()` | not exposed — recursive locks are an anti-pattern; refactor or use one of the binding wrappers |
| `xSemaphoreTake(m, portMAX_DELAY)` | `ove_mutex_lock(m, OVE_WAIT_FOREVER)` |
| `xSemaphoreGive(m)` | `ove_mutex_unlock(m)` |
| `vSemaphoreDelete(m)` | `ove_mutex_destroy(m)` |

## Counting semaphores

| FreeRTOS | oveRTOS |
|---|---|
| `xSemaphoreCreateCounting(max, init)` | `ove_semaphore_create(&s, init, max)` |
| `xSemaphoreTake(s, T)` | `ove_semaphore_take(s, T_ms)` |
| `xSemaphoreGive(s)` | `ove_semaphore_give(s)` |
| `xSemaphoreGiveFromISR(s, &hp_woken)` | `ove_semaphore_give_isr(s)` (no out param; backend handles yield) |

## Queues

| FreeRTOS | oveRTOS |
|---|---|
| `xQueueCreate(len, item_sz)` | `ove_queue_create(&q, item_sz, len)` (note arg order) |
| `xQueueSend(q, &item, T)` | `ove_queue_send(q, &item, T_ms)` |
| `xQueueSendFromISR(q, &item, &woken)` | `ove_queue_send_isr(q, &item)` |
| `xQueueReceive(q, &item, T)` | `ove_queue_receive(q, &item, T_ms)` |
| `uxQueueMessagesWaiting(q)` | `ove_queue_count(q)` |
| `vQueueDelete(q)` | `ove_queue_destroy(q)` |

## Software timers

| FreeRTOS | oveRTOS |
|---|---|
| `xTimerCreate(name, period, autoreload, id, cb)` | `ove_timer_create(&t, name, cb, id, period_ms, OVE_TIMER_{ONESHOT,PERIODIC})` |
| `xTimerStart(t, 0)` | `ove_timer_start(t)` |
| `xTimerStop(t, 0)` | `ove_timer_stop(t)` |
| `xTimerChangePeriod(t, new_period, 0)` | `ove_timer_set_period(t, new_period_ms)` |
| `pvTimerGetTimerID(t)` | `ove_timer_user_data(t)` |

`xTimer*FromISR` variants are not exposed — timer callbacks always run from the timer service thread, and the corresponding shortcuts don't exist on every backend.

## Event groups

| FreeRTOS | oveRTOS |
|---|---|
| `xEventGroupCreate()` | `ove_eventgroup_create(&eg)` |
| `xEventGroupSetBits(eg, mask)` | `ove_eventgroup_set(eg, mask)` |
| `xEventGroupClearBits(eg, mask)` | `ove_eventgroup_clear(eg, mask)` |
| `xEventGroupWaitBits(eg, mask, clear, all, T)` | `ove_eventgroup_wait(eg, mask, &got, flags, T_ms)` |
| `xEventGroupGetBits(eg)` | `ove_eventgroup_get(eg)` |

`flags` is `OVE_EVENT_WAIT_ALL` or `OVE_EVENT_WAIT_ANY`, optionally `| OVE_EVENT_CLEAR_ON_EXIT`.

## Stream buffers

| FreeRTOS | oveRTOS |
|---|---|
| `xStreamBufferCreate(sz, trigger_lvl)` | `ove_stream_create(&s, sz)` |
| `xStreamBufferSend(s, data, len, T)` | `ove_stream_send(s, data, len, T_ms)` |
| `xStreamBufferReceive(s, data, len, T)` | `ove_stream_receive(s, data, len, T_ms)` |
| `xStreamBufferIsEmpty(s)` | `ove_stream_count(s) == 0` |

`trigger_lvl` (the "deliver after N bytes" parameter) is not exposed at the portable API; the reader blocks until at least one byte is available.

## Notifications

FreeRTOS task notifications are FreeRTOS-specific. The portable replacement is a counting semaphore (for value-as-counter notifications) or an event group (for bit-mask notifications).

```c
/* Was: xTaskNotifyGive(h); / ulTaskNotifyTake(pdTRUE, T); */
ove_semaphore_t notify;
ove_semaphore_create(&notify, 0, UINT32_MAX);

/* Producer */
ove_semaphore_give(notify);

/* Consumer */
ove_semaphore_take(notify, OVE_WAIT_FOREVER);
```

## Heap

| FreeRTOS | oveRTOS |
|---|---|
| `pvPortMalloc(size)` | `ove_malloc(size)` (heap mode) — wrapper over `malloc()` |
| `vPortFree(ptr)` | `ove_free(ptr)` |
| `xPortGetFreeHeapSize()` | `ove_heap_free_bytes()` |

In **zero-heap mode**, the heap is not linked. Calls to `ove_malloc` produce a link error. This is intentional.

## Critical sections

| FreeRTOS | oveRTOS |
|---|---|
| `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` | `ove_critical_enter()` / `ove_critical_exit()` |
| `taskENTER_CRITICAL_FROM_ISR()` | `ove_critical_enter_isr()` |
| `portDISABLE_INTERRUPTS()` | `ove_irq_disable()` returning a key; `ove_irq_restore(key)` |

Critical sections are short on every backend. Prefer mutexes for anything > a few dozen instructions.

## Where this leaves you

The translation surface is mostly mechanical. The pieces that take real thought:

- **Allocation discipline** — if you currently mix `xTaskCreate` and `xTaskCreateStatic`, decide whether your oveRTOS port is heap, zero-heap, or both, and stick with the macros for one mode per file.
- **Error handling** — every `ove_*` call returns an `int`. `OVE_OK == 0`. Negative values are `OVE_E_*` codes. There is no equivalent of the configASSERT-on-every-fail path; check return codes explicitly.
- **No `configMINIMAL_STACK_SIZE`** — pass an explicit byte count to `ove_thread_create`; 4096 is a sane minimum.

## See also

- [Architecture Overview](../getting-started/overview.md) — backend dispatch model
- [API Reference](../api/index.md) — full surface for every module
- [Glossary](../glossary.md) — terms unique to oveRTOS
