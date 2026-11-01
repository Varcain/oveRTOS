# Synchronous Rust SPI tests

Run `make test-rust-spi` for heap and zero-heap coverage with actual generated
bindings and measured native storage. The fixture links `src/ove_spi.c` and the
real POSIX mutex implementation. Forwarding spies observe their dependencies;
the test HAL copies actual bounded data and records pointers, lengths, native
word sizes, timeout values and lock/CS ordering. These tests require no device.

The suite exercises unequal/equal/empty transfers, both word types and actual
embedded-hal methods, arbitrary sequences, native errors and resource cleanup.
External Cargo consumers check the ownership and borrow contracts. The separate
`span-regression` binary retains the older call signatures for comparison with
the prior implementation; each case uses valid C-owned backing and sentinel
allocations. Logical slice bounds are checked explicitly by assertions.

`consumer.rs` supplies a normal safe API consumer for firmware integration
checks. Include it as a module in a Rust app, enable its `embedded-hal` dependency
and the ove `embedded-hal` feature, enable SPI, and call `spi_binding_consumer()`
from thread context. It exercises static storage in either mode and native heap
owners where available. Run firmware checks as builds; the example performs SPI
I/O if invoked on a board. Its resource-size symbols support layout measurement.
