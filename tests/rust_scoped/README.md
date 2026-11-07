# Scoped synchronization contracts

Run `make test-rust-scoped` or `make test-rust-scoped-sanitize`. Each gate checks
heap and zero-heap bindings, with and without TIME/ASYNC. It runs the closed
contract cases, persistent early-failure controls, and the two private recursive
reservation tests. The sanitizer gate instruments Rust and the native fixture
with ASan, native C with UBSan, and enables leak detection.

The fixture forwards valid operations to real initialized POSIX primitives.
Independent owner records and entry guards intercept duplicate initialization
or an invalid unlock before it reaches the native primitive. Missing cleanup is
recorded as failure, then independently retired on the same live worker before
its return. Retained native resources have separately counted, quiescent fixture
cleanup. Payload-alias witnesses use `()` and never dereference an access view.

`context_flags` also compiles the actual four IRQ providers against inert native
metadata interfaces. It replaces exactly four architectural reads in a derived
copy of the common helper, preserving its predicates and architecture selection.
Twenty-six profiles cover 212 queries, including restored valid controls.
These prove provider branches and read-only metadata behavior; they do not
execute hardware interrupts or establish a general target timing or stack bound.

Set `OVE_RUST_SCOPED_CANARY=1` to make the permanent gate reject its explicit
failure controls. An ordinary run expects and validates those controls without
accepting an arbitrary failure or a lost failure counter. All case transcripts
and statuses must match, including every ownership and cleanup field.

`target_native` contains separate finite FreeRTOS and Zephyr Cortex-M7 controls.
It uses actual scoped Rust operations and native ownership observations. Its
firmware builds and execution have separate configuration and startup checks.
