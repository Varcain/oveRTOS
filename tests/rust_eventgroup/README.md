# Rust EventGroup contracts

Run `make test-rust-eventgroup` and `make test-rust-eventgroup-sanitize`.

The actual Rust binding runs against a guarded POSIX EventGroup provider in heap
and zero-heap configurations, with EventGroup alone and with the legal optional
TIME/ASYNC prerequisites. The registry rejects duplicate initialization and
unsupported entry attempts before any native effect. Initialization and cleanup
latches exercise claim boundaries without initializing pthread objects twice.
Forgotten and unsupported-context owners remain claimed; test-owned cleanup
retires their native resources only after every worker and callback is quiescent.
It never reopens Rust claims.

The fixture uses ordinary host threads. TLS interrupt/context selection, a narrower
configured mask, distinct ISR returns and forced large-timeout results are inert
routing/error/conversion observers. The optional profile also uses real nested
POSIX critical-section cookies. None is a physical interrupt or arbitrary native
IRQ-priority qualification. Positive signaling, waits, clear-on-exit and callbacks
use actual initialized POSIX primitives and joined workers. Host test allocation
is distinct from zero-heap native owner construction.

Each profile runs ordinary and deliberate early-failure controls. Exact case
lists, totals, process status, individual sidecars and image hashes prevent an
earlier failure from being hidden by later passing checks. The sanitizer lane
instruments Rust and C with ASan, C with nonrecovering UBSan, and enables process
leak detection. Compiler controls use actual generated library artifacts and
check intended diagnostics; compatibility controls are distinguished from newly
repaired guarantees. The actual macro is separately expanded against inert
receivers, so raw dispatch never initializes a native object.

AsyncEventGroup is compiled as an unarmed consumer. Its shared registration API
is outside this ownership/context gate. Relative Duration works without TIME;
absolute methods exist only when TIME is enabled. Saved notify data remains a
separate unsafe lifetime and serialization obligation.
