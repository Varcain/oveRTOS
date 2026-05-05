# Networking Example — C++

Source: `apps/cpp/example_net/src/app.cpp` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_net_cpp_heap/){:target="_blank"}**

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language-specific patterns

This example uses C++20 RAII wrappers. Objects are declared at file scope — constructors handle initialization, destructors handle cleanup. Templates provide compile-time type safety.

See the [overview](index.md) for architecture details and the full API list.

## How to build

```bash
# Native POSIX
make host.posix.example_net_cpp
make configure && make download && make && make run

# WASM (browser)
make wasm.posix.example_net_cpp
make configure && make download && make && make run
```
