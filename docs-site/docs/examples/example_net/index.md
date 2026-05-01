# Networking Example

Exercises the full networking stack with a pass/fail test framework: netif configuration, DNS resolution, TCP/UDP sockets, HTTP client (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and embedded HTTP server.

## Language implementations

| Language | Source | WASM Demo |
|----------|--------|-----------|
| [C](c.md) | `apps/c/example_net/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_net/){:target="_blank"}** |
| [C++](cpp.md) | `apps/cpp/example_net/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_net_cpp/){:target="_blank"}** |
| [Rust](rust.md) | `apps/rust/example_net/` | **[Run in browser](https://varcain.github.io/oveRTOS/example_net_rust/){:target="_blank"}** |
| [Zig](zig.md) | `apps/zig/example_net/` | *Not yet available* |

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
