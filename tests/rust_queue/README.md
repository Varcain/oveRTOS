# Rust Queue contracts

Run `make test-rust-queue` and `make test-rust-queue-sanitize` on an x86-64 host.
`run.py` executes the real Rust owners against the actual POSIX Queue provider
in heap/zero-heap and Queue-only/optional profiles; each fails through its exit
status. IRQ and scheduler phases are TLS branch models. The normal gate also
compiles the lifetime/trait controls, checking each expected rustc rejection
code, and runs the actual queue macro against inert receivers.
