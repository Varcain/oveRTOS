# Synchronous Rust I2C tests

Run `make test-rust-i2c` for heap and zero-heap tests with TIME and CONSOLE
both enabled and both disabled. The latter configurations contain only I2C and
its SYNC dependency. ASYNC and SPI are absent throughout. Bindings and native
storage sizes are generated from each actual configuration.

The fixture links the production I2C frontend, SYNC frontend and POSIX mutexes.
Its observable HAL records complete TX/RX spans, lock ordering, address, speed,
timeout and bounded data. This proves the native call contract, rather than
physical bus waveforms. Ownership tests cover native allocation/free, claimed
static storage, move/drop/reuse, forget, concurrent claims, partial init failure
and cleanup still in progress. Error-path quarantine is separate from the
ordinary real-free test, and guarded dependency entry prevents intentionally
incorrect lifetime mutations from reinitializing or freeing live native objects.

The embedded-hal tests require complete shape validation before dispatch. The
supported shapes are `[]`, one write (including empty), one nonempty read, and
nonempty write followed by nonempty read. Other shapes return `NotSupported`
without effects. Empty reads return `InvalidParam` in otherwise supported
shapes. An empty write prefix followed by read is unsupported, including the
explicit trait `write_read` method. The inherent `write_read` retains its native
empty-prefix plain-read behavior. Empty write and probe reach the native
address/ACK path. These cases are not simulated by separate calls with STOPs.

`type_checks.py` compiles external consumers against the configured production
crate. Copy, Clone, shared bus operations, local backing and unsafe native
wrapping are rejected. Send/Sync rejection preserves the previous contract.
`consumer.rs` is a normal safe firmware consumer requiring no application FFI,
Rust allocator or pinning. Include it in an app with the ove `embedded-hal`
feature and embedded-hal dependency, then call `i2c_binding_consumer` in thread
context. Integration checks build it; invocation performs I2C I/O on a board.
