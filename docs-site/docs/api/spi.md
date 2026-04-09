# SPI — Bus Master Driver

Header: `ove/spi.h` | Kconfig: `CONFIG_OVE_SPI` | Selects: `OVE_SYNC`, `OVE_GPIO`

## Overview

SPI master driver with configurable clock/mode, thread-safe bus locking, and portable chip-select management via GPIO.

## Configuration

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
ove_spi_create(&spi, &cfg);

uint8_t cmd = 0x9F;  // JEDEC ID
uint8_t id[3];
struct ove_spi_xfer xfers[] = {
    { .tx = &cmd, .rx = NULL, .len = 1 },
    { .tx = NULL, .rx = id,   .len = 3 },
};
ove_spi_transfer_seq(spi, &cs, xfers, 2, OVE_WAIT_FOREVER);
```

## Backend Notes

| Backend | Implementation |
|---|---|
| FreeRTOS/STM32 | `HAL_SPI_TransmitReceive()` / `Transmit()` / `Receive()` |
| Zephyr | `spi_transceive()` with `spi_buf_set` |
| NuttX | `/dev/spi*`, `SPIIOC_TRANSFER` ioctl |
| POSIX | `/dev/spidev*`, `SPI_IOC_MESSAGE` ioctl |
