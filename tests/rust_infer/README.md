# Rust inference ownership

`make test-rust-infer test-rust-infer-sanitize` runs the production Rust
binding against initialized synthetic C metadata in both allocation modes.
The sanitizer lane instruments Rust and C with ASan, C with UBSan, and keeps
full leak checking enabled. It needs an environment where LeakSanitizer can
inspect its own processes and threads.

Each contract case has a fresh process. The fixture owns static initialized
backing and atomic observations; it runs no TFLM parser, kernel, native RTOS
allocator, real interrupt or background worker. Retention cases end with their
process and never reset admission or access abandoned handles. Invalid tensor
metadata must be rejected before creating a Rust reference. Constant outputs
remain read-only, and mutable input cannot refer to immutable model bytes.

The external `cargo check` cases prove retained model/storage loans, exclusive
writers, sealed scalar types and explicit qualification/invocation boundaries.
Positive cases cover sequential reuse, all five scalars, Send/shared-reader
Sync, temporary configuration objects and the safe Waker context-transfer route.
Those consumers are compiled only; invalid lifetime/alias/type examples are
never executed. Actual model execution needs its separately qualified program,
input/state domain, arena, target and stack.
