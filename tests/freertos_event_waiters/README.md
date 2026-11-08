# Shared FreeRTOS Event validation

Run `make test-freertos-event-waiters`, `make test-freertos-event-waiters-sanitize`,
`make test-freertos-event-bindings` and `make test-freertos-event-waiters-arm`.
CTest runs the host observers, which compile the actual FreeRTOS Event adapter
and common owners against initialized models. `bindings.py` runs the C++, Rust
and Zig shared waits against that adapter. The ARM gate runs ordinary, MPU V1,
abort and trace profiles of `arm/` on the real FreeRTOS kernel under QEMU.
