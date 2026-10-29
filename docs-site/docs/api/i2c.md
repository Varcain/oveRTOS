# I2C — Bus Master Driver

Header: `ove/i2c.h` | Kconfig: `CONFIG_OVE_I2C` | Selects: `OVE_SYNC`

## Overview

I2C master driver with configurable bus speed, thread-safe bus locking, register-level convenience functions, and device probing. All addresses are 7-bit. Only the FreeRTOS STM32 driver honours `timeout_ns`, in whole milliseconds rounded down (anything under 1 ms times out at once); Zephyr and NuttX ignore it and rely on their drivers' own timeouts. Every call first waits, with no timeout, for the bus lock held by any other transfer on the same bus. A transfer the FreeRTOS STM32 driver abandons on a timeout or an error leaves the bus released. Before the scheduler starts, that driver refuses `ove_i2c_write_read()` with more than two bytes to write (`OVE_ERR_NOT_SUPPORTED`): it runs on interrupts, which FreeRTOS keeps masked until then. `ove_i2c_write_read()` with nothing to write is a plain read.

## Configuration

<!-- ove:body -->
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

<!-- ove:body -->
```c
struct ove_i2c_cfg cfg = { .instance = 0, .speed = OVE_I2C_SPEED_FAST };

ove_i2c_t i2c;
if (ove_i2c_create(&i2c, &cfg) != OVE_OK)
    return;

// Read WHO_AM_I register from an accelerometer at 0x68
uint8_t who_am_i;
if (ove_i2c_reg_read(i2c, 0x68, 0x75, &who_am_i, 1, OVE_WAIT_FOREVER) == OVE_OK) {
    /* who_am_i holds the chip ID */
}

// Scan the bus
for (uint16_t addr = 0x08; addr < 0x78; addr++) {
    if (ove_i2c_probe(i2c, addr, 10) == OVE_OK)
        OVE_LOG_INF("Found device at 0x%02X", addr);
}
```

> Per-backend implementation (which HAL call or `/dev/*` node the wrapper resolves to) is documented under [Internals → Backends → Peripheral wrappers](../backends/index.md#peripheral-wrappers).
