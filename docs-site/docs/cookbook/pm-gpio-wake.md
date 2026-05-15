# Sleep and wake on a GPIO edge

**Pattern** — put the system into a low-power state and wake it on a button press, an interrupt line from a peripheral, or any other digital edge. Uses `ove_pm` to enter sleep and the GPIO subsystem to register the wake source.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_PM=y
  - CONFIG_OVE_GPIO=y
  - CONFIG_OVE_THREAD=y
```

## Sleep states

`ove_pm` exposes a tiered model:

| State | What's running | What survives | Wake latency |
|---|---|---|---|
| `OVE_PM_IDLE` | CPU stopped, peripherals on | All RAM | μs |
| `OVE_PM_SLEEP` | CPU + most peripherals off | All RAM | ms |
| `OVE_PM_DEEP_SLEEP` | CPU + RAM regulator off, backup domain on | Backup SRAM only | ms |
| `OVE_PM_SHUTDOWN` | Everything off | NVS only | reset cycle |

The deeper the state, the longer the wake-up and the less state survives. For "button wakes the board", `OVE_PM_SLEEP` is usually right — RAM is preserved so your app picks up where it left off.

## Code — wake on button (PA0, active low)

```c
#include "ove/ove.h"
#include "ove/log.h"
#include "ove/pm.h"
#include "ove/gpio.h"

OVE_LOG_MODULE_REGISTER(pmgpio);

#define BUTTON_PIN  OVE_GPIO_PIN(0, 0)   /* PA0 on STM32; map per board */

static ove_gpio_t button;

static void button_isr(ove_gpio_t pin, void *arg)
{
    (void)pin; (void)arg;
    /* Just record the event — pm subsystem will resume the main thread. */
    OVE_LOG_INF("button pressed");
}

void ove_main(void)
{
    /* Configure GPIO as input with pull-up (button drives low). */
    struct ove_gpio_config cfg = {
        .mode      = OVE_GPIO_INPUT,
        .pull      = OVE_GPIO_PULL_UP,
        .interrupt = OVE_GPIO_INT_FALLING_EDGE,
    };
    ove_gpio_open(&button, BUTTON_PIN, &cfg);
    ove_gpio_set_callback(button, button_isr, NULL);

    /* Register the GPIO as a wake source.  This survives entering
     * OVE_PM_SLEEP — the pin's EXTI line stays armed in standby. */
    ove_pm_add_wake_source(OVE_PM_WAKE_GPIO, BUTTON_PIN);

    while (1) {
        OVE_LOG_INF("idle for 5 s, then sleeping…");
        ove_thread_sleep_ms(5000);

        OVE_LOG_INF("entering OVE_PM_SLEEP");
        ove_pm_state_set(OVE_PM_SLEEP);
        /* Execution resumes here after wake.  ove_pm_last_wake_reason()
         * tells you why. */
        OVE_LOG_INF("woke (reason=%d)", ove_pm_last_wake_reason());
    }
}
```

## On targets without PM hardware

The POSIX backend treats `ove_pm_state_set(OVE_PM_SLEEP)` as `ove_thread_sleep_ms(INT32_MAX)` and listens for the wake-source pin via the simulator's GPIO transport (`sim/hal/sim_pm.c`). You can run this recipe on the host and trigger the wake by clicking the simulated button in the browser dashboard.

QEMU's mps2-an500 has no real sleep; the call returns immediately and `ove_pm_last_wake_reason()` reports `OVE_PM_WAKE_IMMEDIATE` so you can develop the wake-handling code in the emulator before flashing.

## Wake sources

| Source | Where it comes from |
|---|---|
| `OVE_PM_WAKE_GPIO` | EXTI line on STM32, GPIO interrupt elsewhere |
| `OVE_PM_WAKE_RTC` | RTC alarm — set via `ove_pm_set_wake_after_ms` |
| `OVE_PM_WAKE_UART` | UART data line (one byte arrives) |
| `OVE_PM_WAKE_WATCHDOG` | IWDG bark — emergency wake |

Multiple sources can be armed simultaneously. `ove_pm_last_wake_reason()` reports which fired.

## Periodic wake (without an external trigger)

Use the RTC:

```c
ove_pm_set_wake_after_ms(60 * 1000);   /* wake in 60 s */
ove_pm_state_set(OVE_PM_SLEEP);
/* … resumes here after 60 s or earlier if another source fires … */
```

Combined with NVS for state, this is the foundation of duty-cycled "sample every minute" sensor nodes.

## Where else in the tree

- [API: Power Management](../api/pm.md) — every state, every wake source.
- [`apps/c/heap/example_pm/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_pm) — battery simulation + sleep / wake demo.
- [PM subsystem driver notes (board-side)](../build-system/boards.md) — what each board's BSP wires up.
