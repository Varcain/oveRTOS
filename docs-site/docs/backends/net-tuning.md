# Backend tuning — networking pools

!!! note "Audience"
    This page is for **oveRTOS developers** porting or sizing the networking stack on a specific RTOS. App developers can ignore everything here — the public socket / TLS / HTTP / MQTT API is documented under [API Reference → Networking](../api/net.md).

The protocol stacks (lwIP, mbedTLS, the Zephyr network stack, NuttX networking) own internal allocators. Following the LVGL precedent (`LV_MEM_SIZE` + no libc spillover), each stack is pinned to a BSS pool sized at compile time. Overflow returns `OVE_ERR_NO_MEMORY` rather than spilling to libc — `ove_heap_lock()` post-init catches any regression.

## lwIP (FreeRTOS)

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_OVE_NET_LWIP_MEM_SIZE` | 16384 | lwIP heap (`MEM_SIZE`); BSS-resident, `MEM_LIBC_MALLOC` pinned to 0 |
| `CONFIG_OVE_NET_LWIP_PBUF_POOL_SIZE` | 16 | RX pbuf count (`PBUF_POOL_SIZE`) |
| `CONFIG_OVE_NET_LWIP_PBUF_BUFSIZE` | 1536 | Bytes per pbuf (one MTU) |
| `CONFIG_OVE_NET_LWIP_NUM_TCP_PCB` | 16 | Active TCP connections |
| `CONFIG_OVE_NET_LWIP_NUM_TCP_PCB_LISTEN` | 4 | Listening sockets |
| `CONFIG_OVE_NET_LWIP_NUM_UDP_PCB` | 8 | UDP sockets |
| `CONFIG_OVE_NET_LWIP_NUM_NETCONN` | 16 | Socket-API handles |
| `CONFIG_OVE_NET_LWIP_DNS_TABLE_SIZE` | 4 | DNS cache entries |

## Zephyr net stack

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_OVE_ZEPHYR_NET_MAX_CONN` | 16 | `CONFIG_NET_MAX_CONN` — concurrent TCP/UDP connections |
| `CONFIG_OVE_ZEPHYR_NET_MAX_CONTEXTS` | 16 | `CONFIG_NET_MAX_CONTEXTS` — socket-like contexts |
| `CONFIG_OVE_ZEPHYR_NET_PKT_RX_COUNT` | 14 | RX `net_pkt` slab |
| `CONFIG_OVE_ZEPHYR_NET_PKT_TX_COUNT` | 14 | TX `net_pkt` slab |
| `CONFIG_OVE_ZEPHYR_NET_BUF_RX_COUNT` | 36 | RX `net_buf` payload pool |
| `CONFIG_OVE_ZEPHYR_NET_BUF_TX_COUNT` | 36 | TX `net_buf` payload pool |

## NuttX net stack (zero-heap only)

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_OVE_NUTTX_TCP_CONNS` | 8 | `CONFIG_NET_TCP_PREALLOC_TCBS` |
| `CONFIG_OVE_NUTTX_UDP_CONNS` | 8 | `CONFIG_NET_UDP_PREALLOC_CONNS` |
| `CONFIG_OVE_NUTTX_IOB_NBUFFERS` | 24 | I/O buffer count |
| `CONFIG_OVE_NUTTX_IOB_BUFSIZE` | 196 | Bytes per IOB |

POSIX uses host BSD sockets directly; pool sizing is the host kernel's responsibility, and zero-heap mode on POSIX is a smoke test of the binding control flow only.

## Sizing example

For a device serving ~5 concurrent HTTPS sessions plus MQTT and a small HTTPD:

- TLS: 5 × ~24 KB peak per session → `CONFIG_OVE_NET_TLS_HEAP_SIZE=131072`
- lwIP heap: 5 TLS + 1 MQTT + 4 HTTPD listen + slack → `CONFIG_OVE_NET_LWIP_MEM_SIZE=32768`
- TCP PCBs: same → `CONFIG_OVE_NET_LWIP_NUM_TCP_PCB=12`
- pbufs: 16 RX + outbound TLS chunks → `CONFIG_OVE_NET_LWIP_PBUF_POOL_SIZE=24`
- HTTP response borrow buffer: largest expected response → `CONFIG_OVE_NET_HTTP_MAX_RESPONSE=16384`

Total static cost: ~200 KB, all in BSS, no runtime growth.
