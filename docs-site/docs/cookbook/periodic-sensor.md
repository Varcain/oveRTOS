# Periodic sensor read via timer + queue

**Pattern** — a periodic software timer samples a value (real sensor, simulated, or virtual) and hands it off to a worker thread for processing. Decouples sampling cadence from processing cost.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_LOG_LEVEL_INF=y
  - CONFIG_OVE_QUEUE=y
  - CONFIG_OVE_TIMER=y
  - CONFIG_OVE_THREAD=y
```

## Why a timer, not a thread sleep?

A thread that does `sleep_ms(100); read();` drifts whenever a read takes nonzero time. A timer fires *every* 100 ms regardless of how long the previous callback took (callbacks that overrun get back-pressured by the queue, not by drift).

The timer callback runs in a privileged context (FreeRTOS timer service task; Zephyr system workqueue) — it must not block. So the callback only **enqueues** a sample; the heavy work happens on a worker thread.

## Code

```c
#include "ove/ove.h"
#include "ove/log.h"

#define SAMPLE_PERIOD_MS  100
#define QUEUE_DEPTH        16

typedef struct {
    uint64_t timestamp_us;
    int32_t  value;
} sample_t;

static ove_queue_t  samples;
static ove_timer_t  sample_timer;
static ove_thread_t worker;

/* --- Timer callback: read sensor, enqueue. Must not block. --- */
static void sample_cb(ove_timer_t t, void *arg)
{
    (void)t; (void)arg;

    sample_t s = {
        .timestamp_us = ove_time_now_us(),
        .value        = read_sensor(),   /* your driver */
    };

    /* Non-blocking enqueue (timer context can't wait).  If the worker
     * is falling behind, drop and log.  Timeout is in nanoseconds; 0
     * means no wait. */
    if (ove_queue_send(samples, &s, 0) != OVE_OK) {
        OVE_LOG_WRN("sample dropped (worker behind)");
    }
}

/* --- Worker thread: dequeue, process. May block. --- */
static void worker_fn(void *arg)
{
    (void)arg;
    sample_t s;
    while (1) {
        if (ove_queue_receive(samples, &s, OVE_WAIT_FOREVER) == OVE_OK) {
            process(&s);   /* filter, log, persist, transmit, whatever */
        }
    }
}

void ove_main(void)
{
    ove_queue_create(&samples, sizeof(sample_t), QUEUE_DEPTH);
    ove_thread_create(&worker, "sensor_worker", worker_fn, NULL,
                      OVE_PRIO_NORMAL, 4096);

    /* ove_timer_create(timer, cb, user_data, period_ms, one_shot)
     * — one_shot = 0 for a periodic timer that auto-reloads. */
    ove_timer_create(&sample_timer, sample_cb, NULL,
                     SAMPLE_PERIOD_MS, 0);
    ove_timer_start(sample_timer);

    ove_run();
}
```

## Reading the design

- **Queue depth** caps backlog tolerance. Sized to roughly *(burst length) / (steady-state worker headroom)*. With a 100 ms sample period and an 8 ms worker, 16 deep gives the worker 1.6 seconds to recover from a stall.
- **Drop on full** is the right default for sensor data — newer samples are more useful than queued stale ones. If your domain requires lossless, increase the queue depth and switch the `0` timeout to a small bounded wait (a few ms), but never `OVE_WAIT_FOREVER` inside a timer callback.
- **No mutex needed**: the `ove_queue_t` carries its own synchronization. Pass values by copy through the queue, not pointers into shared state.

## Zero-heap variant

Same shape, file-scope storage:

```c
OVE_QUEUE_DEFINE_STATIC(samples, sizeof(sample_t), QUEUE_DEPTH);
OVE_TIMER_DEFINE_STATIC(sample_timer, sample_cb, NULL,
                        SAMPLE_PERIOD_MS, 0 /* periodic */);
OVE_THREAD_DEFINE_STATIC(worker, 4096, worker_fn, NULL,
                         OVE_PRIO_NORMAL, "sensor_worker");

void ove_main(void) {
    ove_timer_start(sample_timer);
    ove_run();
}
```

The `*_DEFINE_STATIC` macros work in both modes — pick one style per app and stick with it.

## Where else in the tree

- [`apps/c/heap/example/src/app.c`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example/src/app.c) — same producer/consumer/timer trio in the canonical example.
- [API: Timers & Work Queues](../api/timers.md) — timer semantics, periodic vs one-shot, cancellation.
- [API: IPC](../api/ipc.md) — queue, event group, stream buffer.
