# Networking Example — C

Source: `apps/c/example_net/src/app.c` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_net_heap/){:target="_blank"}**

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language-specific patterns

This example uses the heap-mode C API with `_create()` / `_destroy()` calls. For zero-heap builds, switch to `_init()` / `_deinit()` with caller-supplied storage or use `OVE_*_DEFINE_STATIC()` at file scope (the latter also works in heap mode).

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_net
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.example_net
make configure && make download && make && make run
```
