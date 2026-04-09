# I2C — Bus Master Driver

Header: `ove/i2c.h` | Kconfig: `CONFIG_OVE_I2C` | Selects: `OVE_SYNC`

## Overview

I2C master driver with configurable bus speed, thread-safe bus locking, register-level convenience functions, and device probing. All addresses are 7-bit.

## Configuration

```c
struct ove_i2c_cfg {
    unsigned int    instance;  // I2C peripheral index (0, 1, 2...)
    ove_i2c_speed_t speed;     // STANDARD (100kHz), FAST (400kHz), FAST_PLUS (1MHz)
};
```

## API

| Function | Description |
|---|---|
| `ove_i2c_write()` | Write data to a device |
| `ove_i2c_read()` | Read data from a device |
| `ove_i2c_write_read()` | Combined write-then-read with repeated start |
| `ove_i2c_reg_write()` | Write to a single-byte register (portable convenience) |
| `ove_i2c_reg_read()` | Read from a single-byte register (portable convenience) |
| `ove_i2c_probe()` | Probe for device presence (zero-length write, check ACK) |

## Example

```c
struct ove_i2c_cfg cfg = { .instance = 0, .speed = OVE_I2C_SPEED_FAST };

ove_i2c_t i2c;
ove_i2c_create(&i2c, &cfg);

// Read WHO_AM_I register from an accelerometer at 0x68
uint8_t who_am_i;
ove_i2c_reg_read(i2c, 0x68, 0x75, &who_am_i, 1, OVE_WAIT_FOREVER);

// Scan the bus
for (uint16_t addr = 0x08; addr < 0x78; addr++) {
    if (ove_i2c_probe(i2c, addr, 10) == OVE_OK)
        OVE_LOG_INF("Found device at 0x%02X", addr);
}
```

## Backend Notes

| Backend | Implementation |
|---|---|
| FreeRTOS/STM32 | `HAL_I2C_Master_Transmit/Receive()`, `HAL_I2C_Mem_Read()` |
| Zephyr | `i2c_write()`, `i2c_read()`, `i2c_write_read()` |
| NuttX | `/dev/i2c*`, `I2CIOC_TRANSFER` ioctl |
| POSIX | `/dev/i2c-*`, `I2C_RDWR` ioctl |
