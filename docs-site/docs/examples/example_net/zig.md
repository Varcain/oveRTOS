# Networking Example — Zig

Source: `apps/zig/example_net/src/main.zig` | *WASM demo not available — Zig 0.15 lacks wasm32-emscripten support*

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language-specific patterns

This example uses the `ove` Zig module with comptime feature detection (`@hasDecl`), generic types, `defer`-based cleanup, and catch-based error handling.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_net_zig
make configure && make download && make && make run
```
