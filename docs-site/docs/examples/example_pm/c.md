# Power Management Example — C

Source: `apps/c/example_pm/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_pm_c/){:target="_blank"}**

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language-specific patterns

This example uses the unified C API with `_create()` / `_destroy()` calls that work in both heap and zero-heap modes.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_pm_c
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.example_pm_c
make configure && make download && make && make run
```
