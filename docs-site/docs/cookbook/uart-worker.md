# Stream UART input into a worker thread

**Pattern** — a UART receives bytes asynchronously (an LRA, a CLI prompt, an inbound packet protocol). Decouple the ISR-side byte arrival from your protocol parser using a stream buffer.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_UART=y
  - CONFIG_OVE_STREAM=y
  - CONFIG_OVE_THREAD=y
```

## Why a stream buffer, not a queue?

A queue moves *fixed-size messages*. A stream buffer moves *bytes* — perfect for "the next packet might be 7 bytes, the one after 31 bytes". The reader side blocks until at least *N* bytes are available (or a timeout fires), then returns whatever it has.

The UART driver's RX callback fires from an ISR (FreeRTOS) or a high-priority context (Zephyr / NuttX) — it must not block. Pushing bytes into a stream buffer is safe from that context.

## Code

```c
#include "ove/ove.h"
#include "ove/uart.h"
#include "ove/stream.h"
#include "ove/log.h"

OVE_LOG_MODULE_REGISTER(uart_worker);

#define RX_STREAM_SIZE  256

static ove_stream_t rx_stream;
static ove_uart_t   uart;
static ove_thread_t parser;

/* RX callback — runs from an ISR/high-prio context. No blocking. */
static void uart_rx_cb(ove_uart_t u, const uint8_t *data, size_t len, void *arg)
{
    (void)u; (void)arg;
    /* Stream buffer push is interrupt-safe with timeout 0. */
    ove_stream_send(rx_stream, data, len, 0);
}

/* Worker thread — runs in normal thread context, can block. */
static void parser_fn(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    while (1) {
        size_t got = ove_stream_receive(rx_stream, buf, sizeof(buf),
                                        OVE_WAIT_FOREVER);
        if (got > 0) {
            parse_packet(buf, got);   /* your protocol code */
        }
    }
}

void ove_main(void)
{
    ove_stream_create(&rx_stream, RX_STREAM_SIZE);
    ove_thread_create(&parser, "rx_parser", parser_fn, NULL,
                      OVE_PRIO_NORMAL, 4096);

    struct ove_uart_config cfg = {
        .baudrate = 115200,
        .parity   = OVE_UART_PARITY_NONE,
        .stop_bits = 1,
        .flow_ctrl = OVE_UART_FLOW_NONE,
    };
    ove_uart_open(&uart, OVE_UART_DEV_0, &cfg);
    ove_uart_set_rx_callback(uart, uart_rx_cb, NULL);

    ove_run();
}
```

## Reading the design

- **Push side never blocks**. `ove_stream_send(..., 0)` returns immediately. If the stream is full, the call drops bytes and returns the count actually written. Sizing matters; see below.
- **Pull side may block forever**. The parser is happy waiting; `OVE_WAIT_FOREVER` is what you want.
- **Buffer size** = peak burst length × headroom. For 115200 baud (11.5 KB/s) and a parser that consumes every 10 ms, 256 bytes is two parser cycles of margin. Increase if your parser ever stalls (filesystem write, network send, etc.).

## Handling overrun

If `ove_stream_send` returns less than `len`, you lost bytes. For protocols that frame with delimiters, this is recoverable — the parser resyncs on the next delimiter. For protocols that don't frame (binary positional formats), drop the parser state and reconnect.

```c
static void uart_rx_cb(ove_uart_t u, const uint8_t *data, size_t len, void *arg)
{
    size_t wrote = ove_stream_send(rx_stream, data, len, 0);
    if (wrote < len) {
        /* Lost some bytes — signal the parser to resync. */
        atomic_store(&rx_overrun, true);
    }
}
```

## Where else in the tree

- [API: Hardware I/O](../api/io.md) — UART semantics, callbacks, blocking vs non-blocking.
- [API: IPC](../api/ipc.md#stream) — stream buffer reference.
- [`apps/c/heap/example_net/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_net) — uses the same pattern for socket RX.
