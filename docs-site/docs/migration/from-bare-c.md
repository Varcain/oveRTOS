# Migrating from bare-metal C

You have an existing bare-metal firmware (a `while(1)` superloop, optional ISRs, hand-rolled state machines, no RTOS) and want to move to oveRTOS without rewriting everything at once.

Good news: bare-metal code generally ports well. The hard parts are *shared mutable state* (which the superloop hid by serialising everything) and *peripheral ownership* (which becomes explicit once threads exist).

The examples below stay in C so existing bare-metal sources can be retrofitted in place. Once the basic port is working, you can move new code to one of the higher-level bindings — [C++](../examples/example/cpp.md), [Rust](../examples/example/rust.md), or [Zig](../examples/example/zig.md) — without changing the kernel underneath; the application API and the FFI layout stay the same.

## The minimum port

A working superloop usually has this shape:

```c
int main(void)
{
    board_init();
    while (1) {
        poll_sensor();
        update_display();
        process_uart();
    }
}
```

The minimum oveRTOS port keeps the same flow:

```c
#include "ove/ove.h"

void ove_main(void)
{
    /* board_init() ran before us — the bootstrap already happened. */
    while (1) {
        poll_sensor();
        update_display();
        process_uart();
    }
}
```

`ove_run()` is *not* called in this minimal form because we never spawn a kernel object. The framework still ran its preamble (board init, console init if enabled). You now have access to every `ove_*` module from a single thread.

## Step 1 — replace `delay_ms` and busy-loops

Most bare-metal code has at least one `for (volatile int i = 0; i < N; i++);` or `HAL_Delay(ms)`. These spin the CPU.

| Bare-metal | oveRTOS |
|---|---|
| `HAL_Delay(ms)` / `delay_ms(ms)` | `ove_thread_sleep_ms(ms)` |
| `__WFI()` (sleep until interrupt) | `ove_pm_set_state(OVE_PM_STATE_IDLE)` |
| busy-loop on a flag | wait on a binary event, semaphore, or event group |

The replacement does the *same thing semantically* but lets the scheduler put the CPU into a low-power state until your wake event fires.

## Step 2 — split the loop into threads

Once you have more than one thing happening on its own cadence, threads pay off. The split heuristic:

- One thread per **input cadence** (sensor sampling rate, button poll rate, network arrival).
- One thread per **bounded blocking operation** (filesystem write, HTTPS request).
- One thread for **UI** (LVGL handler) if you use it.

Communication goes through queues, not shared variables. The change:

```c
/* Before */
static uint32_t shared_value;

void main_loop(void)
{
    uint32_t v = read_sensor();
    shared_value = v;
    update_display(v);
}

/* After */
static ove_queue_t   q;
static ove_thread_t  sampler;
static ove_thread_t  ui;

static void sampler_fn(void *_) {
    while (1) {
        uint32_t v = read_sensor();
        ove_queue_send(q, &v, 0);
        ove_thread_sleep_ms(50);
    }
}

static void ui_fn(void *_) {
    uint32_t v;
    while (1) {
        if (ove_queue_receive(q, &v, OVE_WAIT_FOREVER) == OVE_OK) {
            update_display(v);
        }
    }
}

void ove_main(void) {
    ove_queue_create(&q, sizeof(uint32_t), 8);
    ove_thread_create(&sampler, "sampler", sampler_fn, NULL,
                      OVE_PRIO_NORMAL, 4096);
    ove_thread_create(&ui, "ui", ui_fn, NULL,
                      OVE_PRIO_NORMAL, 4096);
    ove_run();
}
```

No more shared mutable variable; no more lock; the queue does both.

## Step 3 — move ISRs to deferred work

Bare-metal ISRs often touch a lot of state directly:

```c
void EXTI0_IRQHandler(void) {   /* button pressed */
    update_state_machine();
    redraw_screen();
    clear_interrupt();
}
```

In oveRTOS, ISRs should do the bare minimum and hand off to a thread:

```c
/* Option A: signal a binary event (sem has no ISR-give variant) */
static void button_isr(unsigned int port, unsigned int pin, void *_) {
    ove_event_signal_from_isr(button_evt);
}

/* Option B: post to a work queue (a workqueue handle is required) */
static void button_isr(unsigned int port, unsigned int pin, void *_) {
    ove_work_submit(my_wq, button_work);
}
```

Both options let the scheduler reorder work according to priority, and they let the heavy code (`update_state_machine`, `redraw_screen`) run on a real stack instead of the ISR stack.

The ISR registration goes through `ove_gpio_irq_register(port, pin, mode, isr, user_data)` followed by `ove_gpio_irq_enable(port, pin)`; the underlying NVIC wiring is done by the backend.

## Step 4 — peripherals

You probably hand-rolled `HAL_UART_Transmit`, `HAL_I2C_Master_Transmit`, etc. The oveRTOS wrappers are intentionally close to the same shape:

| Bare-metal (HAL) | oveRTOS |
|---|---|
| `HAL_UART_Init(&huart1)` | `ove_uart_create(&uart, &cfg)` (heap) or `ove_uart_init(&uart, &storage, …)` |
| `HAL_UART_Transmit(&huart1, b, n, T)` | `ove_uart_write(uart, b, n, OVE_MS(ms), NULL)` |
| `HAL_UART_Receive(&huart1, b, n, T)` | `ove_uart_read(uart, b, n, OVE_MS(ms), &got)` |
| `HAL_I2C_Mem_Read(&hi2c1, addr<<1, &reg, 1, b, n, T)` | `ove_i2c_write_read(i2c, addr, &reg, 1, b, n, OVE_MS(ms))` |
| `HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET)` | `ove_gpio_set(port, pin, 1)` |

The big wins:

- **Identical code across STM32, NXP, nRF, RP2040** — only the board file changes.
- **Timeouts are nanoseconds at the API boundary** (`OVE_MS(n)` / `OVE_SEC(n)` helpers or `OVE_WAIT_FOREVER`) — no `pdMS_TO_TICKS` ceremony.
- **DMA / interrupt mode is opt-in via config flags**, not a different function name.

## Things you keep doing the same way

- **Memory layout** — linker scripts, sections, `.noinit`, `.dtcmram` placement all keep working. oveRTOS doesn't impose a new layout.
- **`__attribute__((interrupt))`** functions for ISRs you write directly. The backend's NVIC table picks them up the same way.
- **CMSIS register access** for peripherals oveRTOS doesn't wrap yet. You can mix — there's no "owns the peripheral" enforcement.

## Things to watch out for

- **No more `volatile` discipline for cross-thread communication** — use proper synchronization primitives. The compiler may now reorder operations across `if (flag)` checks. Replace with `ove_eventgroup_set_bits` / `ove_eventgroup_wait_bits`, an event, or a queue.
- **Stack sizes** — bare-metal has one stack (main + ISR). Threaded code needs a stack per thread, sized to the deepest call chain *plus* any ISR that preempts. 4096 bytes is a typical starting point.
- **Priority inversion** — if a low-priority thread holds a mutex and a high-priority thread waits for it, your timing surprise is real. The backend handles priority inheritance for `ove_mutex_*` but not for app-level semaphores.

## See also

- [Quickstart](../getting-started/quickstart.md) — verify the framework works on your machine first
- [Cookbook](../cookbook/index.md) — patterns you'll reach for over and over
- [API Reference](../api/index.md) — full surface
