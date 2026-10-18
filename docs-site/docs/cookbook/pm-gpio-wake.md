# Sleep and wake on a GPIO edge

**Pattern** — let the board drop into its low-power states while nothing happens, and bring it back on a button press, an interrupt line from a peripheral, or any other digital edge. `ove_pm` runs the power-state machine; `ove_gpio` delivers the edge.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_PM=y
  - CONFIG_OVE_GPIO=y
  - CONFIG_OVE_SYNC=y
  - CONFIG_OVE_THREAD=y
```

## How the states work

You do not put the board to sleep by hand. `ove_pm_init()` takes thresholds, and whenever the system is idle the PM layer asks its policy for a state, sleeps in it for about a second (or until the next timer is due), returns to ACTIVE and asks again. The default policy picks:

| State | When |
|---|---|
| `OVE_PM_STATE_ACTIVE` | less than `idle_threshold_ms` since the last activity |
| `OVE_PM_STATE_IDLE` | idle, but a timer is due within `standby_threshold_ms` |
| `OVE_PM_STATE_STANDBY` | idle, nothing due within `standby_threshold_ms` |
| `OVE_PM_STATE_DEEP_SLEEP` | `deep_sleep_threshold_ms` since the last activity |

`ove_pm_activity()` (ISR-safe) records activity and restarts the idle count. A policy callback (`ove_pm_set_policy()`) replaces the table above, and `ove_pm_set_state()` forces a state.

!!! note "What the states do today"
    On every engine and board the states are tracked and reported (`ove_pm_get_stats()`, the notify callbacks), but entering one only parks the PM poller; the CPU sleeps through the RTOS's tickless idle (WFI). No board enters STM32 STOP or STANDBY yet, and `ove_pm_wake_register()` records a wake source without reconfiguring the pin. Because RAM and peripherals always survive, the code below resumes exactly where it left off.

## Code — wake on a button

```c
#include "ove/ove.h"
#include "ove/log.h"
#include "ove/pm.h"
#include "ove/gpio.h"

#define BUTTON_PORT  0    /* board.yaml numbering, as in apps/c/heap/example_pm */
#define BUTTON_PIN   13

static ove_event_t  pressed;
static ove_thread_t worker;

/* Interrupt context: report activity and wake the worker; nothing else. */
static void button_irq(unsigned int port, unsigned int pin, void *arg)
{
    (void)port; (void)pin; (void)arg;
    ove_pm_activity();
    ove_event_signal_from_isr(pressed);
}

static void pm_notify(ove_pm_event_t event, ove_pm_state_t from, ove_pm_state_t to,
                      void *arg)
{
    (void)arg;
    OVE_LOG_INF("%s: %d -> %d",
                event == OVE_PM_EVENT_PRE_SLEEP ? "sleep" : "wake", (int)from, (int)to);
}

static void worker_fn(void *arg)
{
    (void)arg;
    while (1) {
        if (ove_event_wait(pressed, OVE_WAIT_FOREVER) == OVE_OK)
            OVE_LOG_INF("button pressed, state %d", (int)ove_pm_get_state());
    }
}

void ove_main(void)
{
    const struct ove_pm_cfg pm_cfg = {
        .idle_threshold_ms       = 50,
        .standby_threshold_ms    = 5000,
        .deep_sleep_threshold_ms = 30000,
    };
    const struct ove_pm_wake_src button_wake = {
        .type = OVE_PM_WAKE_GPIO,
        .gpio = { .port = BUTTON_PORT, .pin = BUTTON_PIN, .edge = OVE_GPIO_IRQ_FALLING },
    };

    if (ove_event_create(&pressed) != OVE_OK || ove_pm_init(&pm_cfg) != OVE_OK)
        return;
    ove_pm_wake_register(&button_wake);
    ove_pm_notify_register(pm_notify, NULL);

    ove_gpio_configure(BUTTON_PORT, BUTTON_PIN, OVE_GPIO_MODE_INPUT);
    ove_gpio_irq_register(BUTTON_PORT, BUTTON_PIN, OVE_GPIO_IRQ_FALLING, button_irq, NULL);
    ove_gpio_irq_enable(BUTTON_PORT, BUTTON_PIN);

    ove_thread_create(&worker, "button", worker_fn, NULL, OVE_PRIO_NORMAL, 2048);
    ove_run();
}
```

Leave the board alone and the notify callback logs a `sleep: 0 -> 2` / `wake: 2 -> 0` pair about once a second (STANDBY), switching to `0 -> 3` (DEEP_SLEEP) after 30 s. Press the button and the idle count starts over, with the worker logging the press. Log from the callback only while you are learning the pattern: it fires on every one of those cycles.

## Wake sources

| Source | Descriptor field |
|---|---|
| `OVE_PM_WAKE_GPIO` | `.gpio = { port, pin, edge }` |
| `OVE_PM_WAKE_TIMER` | `.timer = { timeout_ms }` |
| `OVE_PM_WAKE_UART` | `.uart = { instance }` |
| `OVE_PM_WAKE_RTC` | `.rtc = { alarm_ms }` |

Register as many as the table holds (`CONFIG_OVE_PM_MAX_WAKE_SOURCES`). What actually wakes the CPU today is any interrupt, as the note above says; the descriptors record your intent for the boards that will gate on them.

## Periodic work without an external trigger

Sleep in the thread that does the work; between wake-ups the PM layer steps down on its own:

```c
#include "ove/ove.h"

void take_sample(void);   /* your code */

static void sampler_fn(void *arg)
{
    (void)arg;
    while (1) {
        take_sample();
        ove_thread_sleep_ms(60 * 1000);
    }
}
```

Combined with NVS for state, this is the shape of duty-cycled "sample every minute" sensor nodes.

## Where else in the tree

- [API: Power Management](../api/pm.md) — every state, every wake source.
- [`apps/c/heap/example_pm/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_pm) — battery simulation + sleep / wake demo.
