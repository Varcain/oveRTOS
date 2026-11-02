# Rust asynchronous I2C checks

`make test-rust-async-i2c` runs real Rust bindings, the native I2C frontend and
POSIX synchronization against a controlled HAL. Heap and zero-heap builds use
only I2C, ASYNC, TIMER, SYNC and TIME. Tests cover independent staged payloads,
copyback, drop/forget, claims, initialization and allocation failures, orphan
results, inline completion/rejection, whole-list traits and cross-core callbacks.
A thread-safe Waker relay accesses its local task only on its owning thread,
between polls. It exercises completion-driven owner Drop and staging growth.
Normal lifecycle cases perform real frees. Retention cases run in isolated
processes and intentionally retain one owner or missing completion; they are
excluded from leak-free assertions. External compile checks verify the safe
public consumer and rejected ownership/borrowing/type contracts.

`make test-rust-async-i2c-arm` runs the ordinary awaited consumer with the native
Embassy executor, actual Cortex-M7 FreeRTOS port, native STM32 provider and
manifest-pinned vendor HAL in QEMU. Register stimuli model peripheral events;
this is kernel/HAL lifetime and heap accounting evidence, not physical wire
validation. The heap budget and timer daemon stack match the native fixture.
No Rust global allocator is installed.

The host fixture stores integer address observations before releasing app
buffers and guards delayed payload access. Premature native backing destruction
and early static-claim-release mutations need stronger lifecycle guards; do not
execute those mutations against this fixture. Conservative transient leases
must not be tested using aliased owner access during an active Future poll.
