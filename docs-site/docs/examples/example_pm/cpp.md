# Power Management Example — C++

Source: `apps/cpp/example_pm/src/app.cpp` | **[Browser demo](demo:example_pm_cpp_heap){:target="_blank"}**

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language-specific patterns

This example uses C++20 RAII wrappers. Objects are declared at file scope — constructors handle initialization, destructors handle cleanup. Templates provide compile-time type safety.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# QEMU + FreeRTOS (with the dashboard)
make qemu.freertos.example_pm_cpp
make configure && make download && make && make run

# Browser (the FreeRTOS firmware in QEMU compiled to WebAssembly)
make web.freertos.example_pm_cpp
make configure && make download && make && make run
```
