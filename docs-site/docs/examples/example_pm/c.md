# Power Management Example — C

Source: `apps/c/example_pm/src/app.c` | **[Browser demo](https://varcain.github.io/oveRTOS/example_pm_c_heap/){:target="_blank"}**

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language-specific patterns

This example uses the heap-mode C API with `_create()` / `_destroy()` calls. For zero-heap builds, switch to `_init()` / `_deinit()` with caller-supplied storage or use `OVE_*_DEFINE_STATIC()` at file scope (the latter also works in heap mode).

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_pm_c
make configure && make download && make && make run

# Browser (the FreeRTOS firmware in QEMU compiled to WebAssembly)
make web.freertos.example_pm_c
make configure && make download && make && make run
```
