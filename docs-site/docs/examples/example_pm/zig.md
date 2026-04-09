# Power Management Example — Zig

Source: `apps/zig/example_pm/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

Demonstrates the PM state machine with automatic idle transitions, peripheral power domain reference counting, wake source registration (GPIO button + UART), custom battery-aware power policy, transition notifications, and runtime power statistics.

## Language-specific patterns

This example uses the `ove` Zig module with comptime feature detection (`@hasDecl`), generic types, `defer`-based cleanup, and catch-based error handling.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_pm_zig
make configure && make download && make && make run
```
