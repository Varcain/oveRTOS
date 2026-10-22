# UART — Serial Bus Driver

Header: `ove/uart.h` | Kconfig: `CONFIG_OVE_UART` | Selects: `OVE_SYNC`, `OVE_STREAM`

## Overview

Multi-instance UART driver with configurable baud rate and framing, interrupt-driven RX buffering (via `ove_stream`), and thread-safe TX.

## Configuration

<!-- ove:body -->
```c
struct ove_uart_cfg {
    unsigned int      instance;      // Peripheral index (0, 1, 2...)
    uint32_t          baudrate;      // Baud rate in bps (e.g. 115200)
    uint8_t           data_bits;     // 7, 8, or 9
    ove_uart_parity_t parity;        // NONE, ODD, EVEN
    ove_uart_stop_t   stop_bits;     // 1, 1.5, 2
    ove_uart_flow_t   flow_control;  // NONE, RTS_CTS
    size_t            rx_buf_size;   // RX ring buffer size in bytes
};
```

## API

| Function | Description |
|---|---|
| `ove_uart_init()` | Initialise with static storage and caller-supplied RX buffer |
| `ove_uart_create()` | Create with heap allocation (or zero-heap macro) |
| `ove_uart_write()` | Blocking write with timeout |
| `ove_uart_read()` | Blocking read from RX buffer with timeout |
| `ove_uart_bytes_available()` | Query bytes in RX buffer |
| `ove_uart_flush()` | Wait, with no timeout, for pending TX bytes to be sent; RX untouched. Zephyr returns at once, so the last byte may still be on the wire. |
| `ove_uart_deinit()` / `ove_uart_destroy()` | Release resources |

## Example

<!-- ove:body -->
```c
struct ove_uart_cfg cfg = {
    .instance    = 0,
    .baudrate    = 115200,
    .data_bits   = 8,
    .parity      = OVE_UART_PARITY_NONE,
    .stop_bits   = OVE_UART_STOP_1,
    .flow_control = OVE_UART_FLOW_NONE,
    .rx_buf_size = 256,
};

ove_uart_t uart;
if (ove_uart_create(&uart, &cfg) != OVE_OK)
    return;

if (ove_uart_write(uart, "Hello\n", 6, OVE_WAIT_FOREVER, NULL) != OVE_OK)
    return;

uint8_t buf[32];
size_t n;
if (ove_uart_read(uart, buf, sizeof(buf), 1000, &n) == OVE_OK) {
    /* n bytes arrived in buf */
}
```

> Per-backend implementation (which HAL call or `/dev/*` node the wrapper resolves to) is documented under [Internals → Backends → Peripheral wrappers](../backends/index.md#peripheral-wrappers).
