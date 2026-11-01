# SPI — Bus Master Driver

Header: `ove/spi.h` | Kconfig: `CONFIG_OVE_SPI` | Selects: `OVE_SYNC`, `OVE_GPIO`

## Overview

SPI master driver with configurable clock/mode, thread-safe bus locking, and portable chip-select management via GPIO. Only the FreeRTOS STM32 driver honours `timeout_ns`, in whole milliseconds rounded down (submillisecond values give a zero HAL wait budget); Zephyr and NuttX ignore it and rely on their drivers' own timeouts. Every call first waits, with no timeout, for the bus lock held by another synchronous transfer through the same ove handle. In `ove_spi_transfer_seq()` the timeout applies to each segment, not the whole sequence.

## Configuration

Transfer lengths count bytes. In 16-bit mode, nonempty lengths must be even and
buffers must be aligned for `uint16_t`. Every non-NULL buffer must cover the full
count. TX/RX buffers may be disjoint or exactly identical for in-place transfers;
partial overlap is rejected. All nonempty sequence records are validated before
software CS or transfer state changes. Zero-length records are skipped.

The mutex serializes synchronous calls through one ove handle. Coordinate
synchronous and async use separately: async does not take that mutex. A sequence retains its supplied
software CS, while native hardware CS and controller locking may end between
segments on NuttX and Zephyr. Coordinate other handles and native driver users
externally. Read-only transmit filler is backend-defined: it may be zero, all
ones, or the initial RX contents. Supply explicit TX data when it matters.

The STM32 FreeRTOS synchronous adapter splits large requests into whole-frame
chunks under one per-segment timeout budget. Its one-shot asynchronous limit is
65535 frames: 65535 bytes in 8-bit mode or 131070 in 16-bit mode. Larger async
requests fail before CS or pending-state publication. These native count checks
do not establish async binding buffer ownership or cancellation safety.

<!-- ove:body -->
```c
struct ove_spi_cfg {
    unsigned int        instance;   // SPI peripheral index (0, 1, 2...)
    uint32_t            clock_hz;   // SCK frequency in Hz
    ove_spi_mode_t      mode;       // MODE_0..MODE_3 (CPOL/CPHA)
    ove_spi_bit_order_t bit_order;  // MSB_FIRST or LSB_FIRST
    uint8_t             word_size;  // 8 or 16 bits
};

struct ove_spi_cs {
    unsigned int gpio_port;  // CS pin GPIO port
    unsigned int gpio_pin;   // CS pin GPIO pin
    int          active_low; // Non-zero = active low (common)
};
```

## API

| Function | Description |
|---|---|
| `ove_spi_transfer()` | Full-duplex transfer (CS held for entire operation) |
| `ove_spi_write()` | TX-only convenience wrapper |
| `ove_spi_read()` | RX-only convenience wrapper |
| `ove_spi_transfer_seq()` | Multi-segment transfer under single CS assertion |

## Example

<!-- ove:body -->
```c
struct ove_spi_cfg cfg = {
    .instance  = 0,
    .clock_hz  = 1000000,
    .mode      = OVE_SPI_MODE_0,
    .bit_order = OVE_SPI_MSB_FIRST,
    .word_size = 8,
};
struct ove_spi_cs cs = { .gpio_port = 0, .gpio_pin = 4, .active_low = 1 };

ove_spi_t spi;
if (ove_spi_create(&spi, &cfg) != OVE_OK)
    return;

uint8_t cmd = 0x9F;  // JEDEC ID
uint8_t id[3];
struct ove_spi_xfer xfers[] = {
    { .tx = &cmd, .rx = NULL, .len = 1 },
    { .tx = NULL, .rx = id,   .len = 3 },
};
if (ove_spi_transfer_seq(spi, &cs, xfers, 2, OVE_WAIT_FOREVER) == OVE_OK) {
    /* id[] holds the manufacturer, type and capacity bytes */
}
```

> Per-backend implementation (which HAL call or `/dev/*` node the wrapper resolves to) is documented under [Internals → Backends → Peripheral wrappers](../backends/index.md#peripheral-wrappers).
