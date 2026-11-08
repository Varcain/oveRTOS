# Rust Queue contracts

Run `make test-rust-queue` and `make test-rust-queue-sanitize` on an x86-64 host.
Both gates execute real Rust owners against the actual POSIX Queue provider and
common heap constructor. They cover heap/zero-heap and Queue-only/optional
profiles. The optional profile enables the existing SYNC, TIME and TIMER
prerequisites of ASYNC. The Queue-only profile enables none of those features.

A separate entry registry intercepts duplicate initialization, unsupported
contexts and invalid cleanup before native effects. Native initialization and
cleanup latches hold one visitor while a competitor checks storage admission;
independent heap objects can still proceed. Initialization errors either occur
before native init or after complete local-handle cleanup, preserving the caller
output. An operation reservation includes the full synchronous callback tail.
Pointers are cleared before backing is freed; generation records are never reset.
No old duplicate native object, escaped payload reference or callback race runs.

Cases exercise claims and raw origins, retry, cleanup/reuse, context/clock
rejection, task versus modeled IRQ routing, occupancy, dimensions, actual FIFO,
finite timeouts, joined producer/consumer and static atomic notification tails.
IRQ and scheduler-phase overrides are TLS branch models, not physical interrupt
or scheduler execution. A real nested POSIX critical section additionally checks
masked admission. Forgotten/unsupported owners retain their resources and claims;
only after every owner, worker, callback and latch visitor is quiescent does the
fixture physically retire the inaccessible native generations for leak checks.
That retirement discards bytes and never reopens a Rust claim or interprets T.

The normal gate also checks 42 compiler-only lifetime/trait controls with actual
generated bindings. Forbidden lifetime bodies never execute. An inert receiver
records two actual macro dispatch observations, allowing old raw/duplicate
expansion to be inspected without creating native resources. These are separate
from the eight native process observations. The sanitizer gate executes eight
process observations with full Rust+C ASan, native UBSan and leak detection;
it does not repeat the metadata-only or inert macro controls.

Each executable retains early failures through successful ordinary controls.
The runner validates complete output and actual status, including its deliberate
expected-failure calibration. `OVE_RUST_QUEUE_CANARY=1` forces the early failure
in every native observation and must make the runner, CLI and Make fail. Clear
the variable to restore the ordinary gate. The CLI reports one aggregate suite.
Logs and sidecars retain statuses, complete output and executable digests.

AsyncQueue setup, other primitive families and hardware benchmarking have
separate acceptance requirements. This target does not qualify shared async
registration or target IRQ priorities.
