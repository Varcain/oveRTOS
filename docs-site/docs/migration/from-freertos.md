# Migrating from raw FreeRTOS

If you're coming from an existing FreeRTOS application (one that calls `xTaskCreate`, `xQueueSend`, etc. directly), this page is the translation table from FreeRTOS-native APIs to the oveRTOS application API.

The tables below are written against the **C surface** because that's the closest analogue to FreeRTOS. The same operations are also available — with idiomatic ergonomics — through the [C++](../examples/example/cpp.md), [Rust](../examples/example/rust.md), and [Zig](../examples/example/zig.md) bindings, and that is the recommended path for new code. Picking a higher-level binding is a per-project decision; the kernel choice (FreeRTOS, Zephyr, NuttX) is independent.

Three differences worth knowing up front:

1. **Allocation model** — FreeRTOS has heap-and-static at the API surface (`xTaskCreate` vs `xTaskCreateStatic`); oveRTOS has heap-and-static at the *build configuration* level (`_create()` vs `OVE_*_DEFINE_STATIC()` driven by `CONFIG_OVE_ZERO_HEAP`).
2. **Error reporting** — FreeRTOS returns `pdPASS` / `pdFAIL` / typed values; oveRTOS returns a uniform `OVE_OK` / negative-on-error pattern across every module. The C++ binding wraps this in exceptions-as-values; Rust returns `Result<T, Error>`; Zig returns `Error!T`.
3. **Time units** — every blocking call takes a timeout in **nanoseconds** as a `uint64_t`. Use the `OVE_MS(n)` / `OVE_SEC(n)` helpers from `ove/types.h`, or `OVE_WAIT_FOREVER` for an unbounded wait. Plain millisecond `uint32_t`s appear only on duration-style APIs (`ove_thread_sleep_ms`, `ove_timer_create` period).

## Threads (tasks)

| FreeRTOS | oveRTOS (heap mode) | oveRTOS (zero-heap mode) |
|---|---|---|
| `xTaskCreate(fn, name, stack, arg, prio, &h)` | `ove_thread_create(&h, name, fn, arg, prio, stack)` | `OVE_THREAD_DEFINE_STATIC(h, stack, fn, arg, prio, name)` |
| `xTaskCreateStatic(fn, name, stack, arg, prio, stackbuf, &tcb)` | n/a — use the `OVE_THREAD_DEFINE_STATIC` macro | same macro |
| `vTaskDelete(h)` | cooperative `ove_thread_request_stop(h)` + `ove_thread_destroy(h)` | cooperative `ove_thread_request_stop(h)` + `ove_thread_deinit(h)` |
| `vTaskDelay(pdMS_TO_TICKS(ms))` | `ove_thread_sleep_ms(ms)` | same |
| `taskYIELD()` | `ove_thread_yield()` | same |
| `xTaskGetCurrentTaskHandle()` | `ove_thread_get_self()` (NULL on tasks oveRTOS did not create) | same |
| `vTaskPrioritySet(h, p)` | `ove_thread_set_priority(h, p)` | same |

Priority levels map: `tskIDLE_PRIORITY` → `OVE_PRIO_IDLE`, and higher numbers win, as in FreeRTOS. oveRTOS priorities run from 0 to `OVE_PRIO_MAX` (31) with named anchors (`OVE_PRIO_LOW`, `OVE_PRIO_NORMAL`, `OVE_PRIO_HIGH`, `OVE_PRIO_CRITICAL`, …); `OVE_PRIO_ABOVE(p, n)` and `OVE_PRIO_BELOW(p, n)` reach the levels between them. On FreeRTOS each ove priority is the native priority of the same number. Use the names: Zephyr and NuttX number their priorities differently (lower wins in Zephyr), and the anchors keep their meaning on every backend.

There is no portable getter for priority — keep your own copy if you need to read it back.

Unlike `vTaskDelete`, oveRTOS teardown does not forcibly terminate a worker:
`destroy` and `deinit` join after its entry returns. A blocked cooperative worker
must also be woken so it can observe the stop request.

## Mutexes

| FreeRTOS | oveRTOS |
|---|---|
| `xSemaphoreCreateMutex()` | `ove_mutex_create(&m)` |
| `xSemaphoreCreateRecursiveMutex()` | `ove_recursive_mutex_create(&m)` |
| `xSemaphoreTake(m, portMAX_DELAY)` | `ove_mutex_lock(m, OVE_WAIT_FOREVER)` |
| `xSemaphoreTake(m, pdMS_TO_TICKS(ms))` | `ove_mutex_lock(m, OVE_MS(ms))` |
| `xSemaphoreGive(m)` | `ove_mutex_unlock(m)` |
| `vSemaphoreDelete(m)` | `ove_mutex_destroy(m)` |

## Counting semaphores

| FreeRTOS | oveRTOS |
|---|---|
| `xSemaphoreCreateCounting(max, init)` | `ove_sem_create(&s, init, max)` |
| `xSemaphoreTake(s, pdMS_TO_TICKS(ms))` | `ove_sem_take(s, OVE_MS(ms))` |
| `xSemaphoreGive(s)` | `ove_sem_give(s)` |
| `xSemaphoreGiveFromISR(s, &hp_woken)` | **no ISR variant for `ove_sem_give`** — use a binary event (`ove_event_signal_from_isr`) or an event group (`ove_eventgroup_set_bits_from_isr`) for ISR→thread signalling |
| `vSemaphoreDelete(s)` | `ove_sem_destroy(s)` |

## Queues

| FreeRTOS | oveRTOS |
|---|---|
| `xQueueCreate(len, item_sz)` | `ove_queue_create(&q, item_sz, len)` (note arg order) |
| `xQueueSend(q, &item, pdMS_TO_TICKS(ms))` | `ove_queue_send(q, &item, OVE_MS(ms))` |
| `xQueueSendFromISR(q, &item, &woken)` | `ove_queue_send_from_isr(q, &item)` (no out param; backend handles yield) |
| `xQueueReceive(q, &item, pdMS_TO_TICKS(ms))` | `ove_queue_receive(q, &item, OVE_MS(ms))` |
| `xQueueReceiveFromISR(q, &item, &woken)` | `ove_queue_receive_from_isr(q, &item)` |
| `vQueueDelete(q)` | `ove_queue_destroy(q)` |

## Software timers

`ove_timer_create()` takes the timer object directly — no name, no autoreload enum. The "one-shot" toggle is a plain `int` (non-zero → one-shot, zero → periodic). Period is in **milliseconds**.

| FreeRTOS | oveRTOS |
|---|---|
| `xTimerCreate("t", period_ticks, autoreload, id, cb)` | `ove_timer_create(&t, cb, user_data, period_ms, 0 /* periodic */)` |
| `xTimerStart(t, 0)` | `ove_timer_start(t)` |
| `xTimerStop(t, 0)` | `ove_timer_stop(t)` |
| `xTimerReset(t, 0)` | `ove_timer_reset(t)` |
| `xTimerDelete(t, 0)` | `ove_timer_destroy(t)` |

The callback signature is `void cb(ove_timer_t t, void *user_data)` — the timer handle plus the opaque pointer captured at creation. `xTimer*FromISR` variants are not exposed; timer callbacks always run from the timer service thread, and the corresponding shortcuts don't exist on every backend.

## Event groups

`ove_eventgroup_*` operations use the `_bits` suffix and `_from_isr` for the ISR variant. The wait function returns through an out-parameter, with the timeout placed *before* the result pointer.

| FreeRTOS | oveRTOS |
|---|---|
| `xEventGroupCreate()` | `ove_eventgroup_create(&eg)` |
| `xEventGroupSetBits(eg, mask)` | `ove_eventgroup_set_bits(eg, mask)` |
| `xEventGroupSetBitsFromISR(eg, mask, &woken)` | `ove_eventgroup_set_bits_from_isr(eg, mask)` |
| `xEventGroupClearBits(eg, mask)` | `ove_eventgroup_clear_bits(eg, mask)` |
| `xEventGroupClearBitsFromISR(eg, mask)` | `ove_eventgroup_clear_bits_from_isr(eg, mask)` (still deferred to the timer task) |
| `xEventGroupWaitBits(eg, mask, clear, all, T)` | `ove_eventgroup_wait_bits(eg, mask, flags, OVE_MS(ms), &got)` |
| `xEventGroupGetBits(eg)` | `ove_eventgroup_get_bits(eg)` |

`flags` is `0` (wake when any requested bit is set) or `OVE_EG_WAIT_ALL`, optionally `| OVE_EG_CLEAR_ON_EXIT`.

Use `ove_eventgroup_valid_mask()` instead of an all-ones literal. The configured FreeRTOS high control byte is excluded before narrowing: 16-bit ticks allow 8 payload bits, 32-bit ticks allow 24, and a 64-bit native event type allows all 32 public bits. Invalid scalar set/clear masks return zero without native effects; ISR clear returns `OVE_ERR_INVALID_PARAM`. Waits reject zero masks, unsupported bits and unknown flags with `OVE_ERR_INVALID_PARAM`, leaving the result unchanged. Zero set/clear masks remain valid. Task clear returns the full prior value. ISR set reports its accepted mask or zero on queue rejection and takes effect later in the timer task.

Quiesce EventGroup producers/interrupt sources and synchronous users before teardown. Pending oveRTOS deferred commands are canceled before the storage is freed or reused. Nonzero ISR sets notify from the timer daemon after publication; callback code/data must outlive pending and saved delivery tails, including across unregister/deinit. Keep task getters and state toggles in task context; an ISR can signal a thread to update state.

## Stream buffers

`ove_stream_create()` carries the `trigger` argument (the "deliver after N bytes" threshold) directly into the constructor instead of into the buffer-create function only on FreeRTOS.

| FreeRTOS | oveRTOS |
|---|---|
| `xStreamBufferCreate(sz, trigger_lvl)` | `ove_stream_create(&s, sz, trigger_lvl)` |
| `xStreamBufferSend(s, data, len, T)` | `ove_stream_send(s, data, len, OVE_MS(ms), &sent)` |
| `xStreamBufferReceive(s, data, len, T)` | `ove_stream_receive(s, data, len, OVE_MS(ms), &received)` |
| `vStreamBufferDelete(s)` | `ove_stream_destroy(s)` |

## Notifications

FreeRTOS task notifications are FreeRTOS-specific. The portable replacement is a binary event (for one-shot wakeups) or a counting semaphore (for value-as-counter notifications).

```c
/* Was: xTaskNotifyGive(h); / ulTaskNotifyTake(pdTRUE, T); */
ove_sem_t notify;
ove_sem_create(&notify, 0, UINT32_MAX);

/* Producer */
ove_sem_give(notify);

/* Consumer */
ove_sem_take(notify, OVE_WAIT_FOREVER);
```

## Heap

The portable API does not currently expose `malloc` / `free`. In **heap mode**, the framework owns the heap and you allocate implicitly via the `_create()` constructors. In **zero-heap mode** (`CONFIG_OVE_ZERO_HEAP=y`), `_create()` is link-gated out and a runtime trap fires on any post-init allocation (see `backends/common/ove_heap_lock.c`).

| FreeRTOS | oveRTOS |
|---|---|
| `pvPortMalloc(size)` | not exposed — design for caller-owned storage (`_init`, `OVE_*_DEFINE_STATIC`) or use `_create()` and trust the implicit heap |
| `vPortFree(ptr)` | not exposed — destroy via the matching `_destroy()` |
| `xPortGetFreeHeapSize()` | not exposed — backend-specific; query the RTOS directly if needed |

## Critical sections

With `CONFIG_OVE_ASYNC`, `ove_irq_lock()` / `ove_irq_unlock()` provide paired,
nestable native critical sections. Pass the complete opaque key back in LIFO
order in the same execution context. C++ and Zig use the same C contract; Rust's
`critical_section::with` and `SpinMutex::with` use the selected provider.

On supported single-core FreeRTOS Cortex-M ports, this excludes kernel-aware
participants at the configured syscall threshold. Higher-urgency IRQs remain
runnable and cannot participate in that protection. Unsupported FreeRTOS entry
stops before returning; restricted MPU V1 tasks additionally need a serviceable
priority-zero supervisor call with PRIMASK/FAULTMASK clear.

The wrapper preserves its own transitions and restores captured masks on unlock.
A native call inside the body can still lower stronger incoming BASEPRI or clear
PRIMASK; restoration afterwards cannot undo execution during that interval.
See [IRQ critical sections](../api/sync.md#irq-critical-sections) for the precise
supported contexts and an example of this limitation.

Use a mutex for blocking thread synchronization and eligible ISR handoff APIs
for deferred work. Critical-section bodies must stay short and nonblocking.

## Where this leaves you

The translation surface is mostly mechanical. The pieces that take real thought:

- **Allocation discipline** — if you currently mix `xTaskCreate` and `xTaskCreateStatic`, decide whether your oveRTOS port is heap, zero-heap, or both, and stick with the macros for one mode per file.
- **Error handling** — every `ove_*` call returns an `int`. `OVE_OK == 0`. Negative values are `OVE_ERR_*` codes. Check return codes explicitly; there is no `configASSERT`-on-every-fail path.
- **No `configMINIMAL_STACK_SIZE`** — pass an explicit byte count to `ove_thread_create`; 4096 is a sane minimum.

## See also

- [Architecture Overview](../getting-started/overview.md) — backend dispatch model
- [API Reference](../api/index.md) — full surface for every module
- [Glossary](../glossary.md) — terms unique to oveRTOS
