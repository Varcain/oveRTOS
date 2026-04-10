# Power Management Example

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language implementations

| Language | Source | WASM Demo |
|----------|--------|-----------|
| [C](c.md) | `apps/c/example_pm/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_pm_c/){:target="_blank"}** |
| [C++](cpp.md) | `apps/cpp/example_pm/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_pm_cpp/){:target="_blank"}** |
| [Rust](rust.md) | `apps/rust/example_pm/` | *Not available* |
| [Zig](zig.md) | `apps/zig/example_pm/` | *Not yet available* |

## Key APIs demonstrated

ove_pm_init, ove_pm_set_state, ove_pm_activity, ove_pm_wake_register, ove_pm_domain_request/release, ove_pm_set_policy, ove_pm_notify_register, ove_pm_get_stats, ove_pm_set_budget

## Required Kconfig

`CONFIG_OVE_PM=y`

## How to build

```bash
make host.posix.example_pm_c      # C
make host.posix.example_pm_cpp  # C++
make configure && make download && make && make run
```
