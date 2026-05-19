# Networking Example

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language implementations

| Language | Source | WASM Demo |
|----------|--------|-----------|
| [C](c.md) | `apps/c/heap/example_net/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_net_heap/){:target="_blank"}** |
| [C++](cpp.md) | `apps/cpp/heap/example_net/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_net_cpp_heap/){:target="_blank"}** |
| [Rust](rust.md) | `apps/rust/heap/example_net/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_net_rust_heap/){:target="_blank"}** |
| [Zig](zig.md) | `apps/zig/heap/example_net/` | *Not yet available* |

Zero-heap variants live under `apps/<lang>/zeroheap/example_net/`.

## Key APIs demonstrated

ove_netif_*, ove_socket_*, ove_dns_resolve, ove_http_*, ove_sntp_*, ove_mqtt_*, ove_httpd_*

## Required Kconfig

`CONFIG_OVE_BSP=y, CONFIG_OVE_NET=y, CONFIG_OVE_NET_HTTP=y, CONFIG_OVE_NET_MQTT=y, CONFIG_OVE_NET_SNTP=y, CONFIG_OVE_NET_HTTPD=y`

## How to build

```bash
make host.posix.example_net      # C
make host.posix.example_net_cpp  # C++
make configure && make download && make && make run
```
