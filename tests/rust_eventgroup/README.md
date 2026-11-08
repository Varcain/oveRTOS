# Rust EventGroup contracts

Run `make test-rust-eventgroup` and `make test-rust-eventgroup-sanitize`.
`run.py` executes the actual Rust binding against a guarded POSIX EventGroup
provider in heap/zero-heap and EventGroup-only/optional profiles; each fails
through its exit status. TLS interrupt/context selection is an inert routing
model, not a physical interrupt. The normal gate also compiles the ownership and
feature callers, checking each expected rustc code, and runs the actual macro
against inert receivers.
