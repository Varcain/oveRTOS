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

### Synchronous Rust ownership and traits

Use ordinary Rust configuration and an exclusive owner; application FFI and a
Rust allocator are unnecessary. Heap builds support `I2c::new(Config { ... })`.
Static storage works in either allocation mode:

```rust
use core::time::Duration;
use ove::{I2cStorage, i2c::{Config, Speed}};
static BUS: I2cStorage = I2cStorage::new();

fn read_sensor() -> ove::Result<()> {
    let mut bus = BUS.init(Config { instance: 0, speed: Speed::Fast })?;
    let mut data = [0; 7];
    bus.write_read(0x42, &[1, 2, 3], &mut data, Duration::from_millis(20))?;
    Ok(())
}
```

Drop completes native cleanup before static reuse. A live or forgotten static
owner keeps its claim, so another initialization returns `Busy`. Owners move
without moving their native backing and are neither Copy/Clone nor Send/Sync.
Unsafe `from_handle` wrappers remain nonowning and require external lifetime,
exclusivity and quiescence coordination. Independent handles still need physical
bus coordination. Raw async integration must finish before owned cleanup.

With the `embedded-hal` feature, supported transaction shapes are an empty
list, one write (including empty), one nonempty read, and nonempty write followed
by nonempty read. Other shapes return `NotSupported` before effects. Empty reads
in otherwise supported shapes return `InvalidParam`. An empty write followed by
read, including explicit trait `write_read`, is unsupported because a plain read
cannot preserve its write address phase. Inherent `write_read` keeps its native
empty-prefix plain-read convenience. Pure empty writes/probes preserve native
address/ACK behavior. Trait addresses are validated even for an empty list.

I2C's relative durations work without `CONFIG_OVE_TIME`. SYNC APIs using a clock
or absolute deadline require TIME; relative single-call waits remain available.
Logging with CONSOLE disabled produces no output. These feature boundaries do
not enable either optional module or change the native transfer timeout rules.

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

### Owned asynchronous Rust I2C

Enable the Rust `async` feature and native I2C, ASYNC, TIMER, SYNC and TIME.
SPI, GPIO and CONSOLE are unnecessary. Use `AsyncI2c::new(Config)` in heap
builds, or claimed static storage in either allocation mode:

```rust
use ove::{AsyncI2cStorage, i2c::Config};
static BUS: AsyncI2cStorage<3, 7> = AsyncI2cStorage::new();

async fn read_sensor() -> ove::Result<()> {
    let mut bus = BUS.init(Config::default())?;
    let mut rx = [0; 7];
    bus.write_read(0x42, &[1, 2, 3], &mut rx).await?;
    bus.flush().await?;
    Ok(())
}
```

The binding owns stable native backing and independent TX/RX byte staging;
applications need no FFI, Rust allocator or manual pinning. Operations borrow
`&mut self`. Owners and operation futures are neither Send nor Sync, and owners
are neither Copy nor Clone. Moving an owner leaves native memory in place.
Static capacity defaults to equal TX/RX sizes; either direction may have zero
capacity. Exceeding either returns `NoSpace` before I/O. Heap staging grows
fallibly, preserving each direction's high-water capacity and allocating the
new pair before freeing the old pair. Native alignment overhead also applies.

Drop or forget of a future releases application buffers without cancellation;
native code retains only internal spans. Only a successful live future copies
RX back. New work returns `BusBusy` while an orphan is active. Once complete,
new work may replace its result; `flush().await` instead consumes the actual
result without copyback. Owner Drop defers cleanup through native callback,
wake and submission tails. A missing completion or forgotten owner retains one
bounded context and its capacities/claim. No automatic watchdog, splitting or
filler is introduced. Independent handles still require physical-bus coordination.

The `embedded-hal-async` seven-bit trait supports an empty no-op list, one
nonempty write/read, or one nonempty write followed by one nonempty read in one
native transaction. Other whole lists return `NotSupported` before effects.
Empty single phases and a nonempty write with empty read return `InvalidParam`; an empty
write prefix before a read returns `NotSupported`, including trait
`write_read`. Inherent `write_read` uses a plain read for empty TX, pure write
for empty RX, and rejects both empty. Empty trait lists validate the address
and do not flush an orphan. Provider limits and NuttX native worker/reserve
qualifications described above still apply.
