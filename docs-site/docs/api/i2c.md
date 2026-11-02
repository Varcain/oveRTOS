# I2C — Bus Master Driver

Header: `ove/i2c.h` | Kconfig: `CONFIG_OVE_I2C` | Selects: `OVE_SYNC`

## Overview

I2C master driver with configurable bus speed, thread-safe bus locking, register-level convenience functions, and device probing. All addresses are 7-bit. Only the FreeRTOS STM32 driver honours `timeout_ns`, in whole milliseconds rounded down (anything under 1 ms times out at once); Zephyr and NuttX ignore it and rely on their drivers' own timeouts. After validation, synchronous calls wait without a timeout for the bus lock held by another synchronous transfer through the same handle. A transfer the FreeRTOS STM32 driver abandons on a timeout or an error leaves the bus released. Before the scheduler starts, that driver refuses `ove_i2c_write_read()` with more than two bytes to write (`OVE_ERR_NOT_SUPPORTED`): it runs on interrupts, which FreeRTOS keeps masked until then. `ove_i2c_write_read()` with nothing to write is a plain read.


Addresses above `0x7f` return `OVE_ERR_INVALID_PARAM` before locking, I/O or async
publication. TX and RX have independent byte counts. STM32 FreeRTOS accepts up
to 65535 bytes in each phase, Zephyr requires each to fit `uint32_t`, and NuttX
requires each to fit both `ssize_t` and the supported F7 driver's `int` counter.
Oversize requests fail before effects; transfers are never split to bypass a
limit. Register writes include their extra prefix byte in the count.

Zero-byte synchronous writes and probes retain the native address/ACK operation.
A zero TX length in write-read selects a plain read. Async write-read also
supports pure write when RX is empty, rejects both empty, and has no timeout
argument or automatic watchdog. NuttX combined transfers mark the first message
with `I2C_M_NOSTOP` to preserve the public repeated-start contract.

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

### Asynchronous completion and lifetime

Submit, prepare, deinit and destroy from thread context. Final I2C callbacks run in task context after native submission and provider
retirement. FreeRTOS uses its existing timer daemon after the HAL interrupt
returns; Zephyr dispatches after the transfer work's kernel cleanup; NuttX uses
the final worker or submitter access. The internal write-to-read transition stays
in the STM32 interrupt. Callbacks must be non-blocking. Replace unconditional
FromISR-only callback calls with task APIs.

Deinit/destroy consume the handle and defer cleanup for accepted work. Caller
storage, TX/RX buffers and userdata must survive both callback completion and
every active API call returning. Callback notification does not join an
independent concurrent close call. Only a live unconsumed handle may be reused
from a callback. Rejected submissions produce no callback. A missing completion
retains one controller; async submission has no automatic watchdog.

Preparation is automatic during submission; `ove_i2c_async_prepare()` can establish
the completion service beforehand. The FreeRTOS and Zephyr services retain a
small permanent list/timer and retry every 10ms, using existing tasks and bounded
drain batches. Queue/service starvation has no hard completion deadline. NuttX
release zero-heap uses the existing native reserve for workers; debug zero-heap
returns NotSupported before admission because its debug heap lock rejects native
worker allocation. Native thread resource reclamation may outlast notification.
