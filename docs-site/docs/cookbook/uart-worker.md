# Stream UART input into a worker thread

**Pattern** — a UART receives bytes asynchronously (a GPS, a CLI prompt, an inbound packet protocol). A worker thread blocks on the UART and hands each chunk to your parser; the driver's own receive buffer absorbs the bursts.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_UART=y
  - CONFIG_OVE_THREAD=y
```

## Where the bytes wait

You don't write an RX interrupt handler. Each backend's UART ISR pushes received bytes into a byte stream inside the driver (an `ove_stream` of `rx_buf_size` bytes, `CONFIG_OVE_UART` selects `CONFIG_OVE_STREAM`). `ove_uart_read()` blocks until at least one byte is there, then returns whatever is buffered, up to the length you asked for. A queue would move fixed-size messages; a byte stream suits "the next packet might be 7 bytes, the one after 31".

## Code

```c
#include "ove/ove.h"
#include "ove/uart.h"
#include "ove/log.h"

#define RX_BUF_SIZE  256

void parse_packet(const uint8_t *data, size_t len);   /* your protocol code */

static ove_uart_t   uart;
static ove_thread_t parser;

/* Worker thread — normal thread context, may block. */
static void parser_fn(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    while (1) {
        size_t got = 0;
        if (ove_uart_read(uart, buf, sizeof(buf), OVE_WAIT_FOREVER, &got) == OVE_OK &&
            got > 0) {
            parse_packet(buf, got);
        }
    }
}

void ove_main(void)
{
    const struct ove_uart_cfg cfg = {
        .instance     = 1,          /* a free UART; 0 is usually the console */
        .baudrate     = 115200,
        .data_bits    = 8,
        .parity       = OVE_UART_PARITY_NONE,
        .stop_bits    = OVE_UART_STOP_1,
        .flow_control = OVE_UART_FLOW_NONE,
        .rx_buf_size  = RX_BUF_SIZE,
    };
    if (ove_uart_create(&uart, &cfg) != OVE_OK) {
        OVE_LOG_ERR("UART %u unavailable", cfg.instance);
        return;
    }
    if (ove_thread_create(&parser, "rx_parser", parser_fn, NULL,
                          OVE_PRIO_NORMAL, 4096) != OVE_OK)
        return;
    ove_run();
}
```

## Reading the design

- **The read may return less than you asked for.** `got` is however many bytes were buffered (at least one). Your parser keeps its own state across calls; a packet can arrive split over several reads.
- **The parser can wait forever.** `OVE_WAIT_FOREVER` is the right timeout when nothing else is due; pass a finite timeout (nanoseconds) to wake up periodically, e.g. for an inter-byte timeout in a framed protocol.
- **Buffer size** = peak burst length × headroom. At 115200 baud (11.5 KB/s), a parser that runs every 10 ms needs about 115 bytes per cycle, so 256 bytes is two cycles of margin. Increase it if the parser ever stalls (filesystem write, network send, etc.).
- **Writing back** is `ove_uart_write(uart, data, len, timeout_ns, &written)`; it is thread-safe and may accept fewer than `len` bytes, so loop until everything is sent.

## Handling overrun

When the receive buffer is full, the driver drops the bytes that do not fit; there is no overrun counter to read. For protocols that frame with delimiters this is recoverable: the parser resyncs on the next delimiter. For protocols that don't frame (binary positional formats), add a length and checksum to each packet so the parser can detect a gap, drop its state and wait for the next start marker. If drops happen in practice, raise `rx_buf_size` or the parser thread's priority.

For a zero-heap build, `OVE_UART_DEFINE_STATIC(name, rx_buf_sz, cfg_ptr)` declares the storage and RX buffer at file scope and initialises the UART before `ove_main()` runs; drop the `ove_uart_create()` call.

## Where else in the tree

- [API: Hardware I/O](../api/io.md) — UART semantics, callbacks, blocking vs non-blocking.
- [API: IPC](../api/ipc.md#stream) — stream buffer reference.
- [`apps/c/heap/example_net/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_net) — uses the same pattern for socket RX.
