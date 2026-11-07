# Rust semaphore and event contracts

The executable uses the configured Rust bindings and generated native storage
sizes. Valid operations forward to the real POSIX synchronization adapter and
common heap constructors. An entry registry intercepts duplicate initialization,
invalid context and invalid cleanup before native effects. Initialization and
teardown latches admit one visitor while competitors inspect the recorded state.
Each allocation or static initialization has a new registry generation. Inactive
records retain integer address snapshots; pointers are cleared before backing
can be freed. The registry and Rust failure accumulator are never reset.

Ordinary cases cover static admission, overlapping constructor/teardown calls,
heap independence, error cleanup, exclusive raw origin, same-site macro reuse,
cross-thread handoff, and actual POSIX post/wait behavior. Context metadata and
the optional deterministic deadline clock are synthetic branch observations.
No real interrupt runs pthread operations. The cleaned-error selector completes
actual native initialization and cleanup before returning an error with its
initialized output pointer unchanged. Rust must ignore that pointer on error.

Forgotten and unsupported-context dropped owners intentionally retain native
resources. The cases measure that retention and Busy/no-cleanup behavior before
the fixture retires quiescent generations. Workers are joined, latches released
and all accessible owners gone before this independent cleanup; it never reopens
forgotten Rust claims. Raw extents remain static through retirement. The outer
heap destroy reservation includes its common deinit/free path, and active native
calls exclude physical teardown. Native initialization/cleanup must balance with
full leak checking after retirement. A canary
records an early named failure, runs ordinary controls and retains that failure
in its summary and exit status. Complete output and actual status are required
for gate acceptance. Duplicate macro dispatch needs a separate inert-receiver
check; production macro panic is never an expected failure witness.

This target covers Semaphore and Event only. Mutex, RecursiveMutex, CondVar,
other synchronization families and AsyncSemaphore callback registration need
their own ownership and concurrency proofs. Native firmware builds provide
layout and integration evidence; they do not qualify execution by themselves.

Minimal configuration needs SYNC alone. The optional TIME/ASYNC profile uses
the actual POSIX IRQ-depth provider and the unchanged ove critical-section
implementation, compiled as a test module with the real generated FFI types.
Actual nested critical-section Event posts and pinned Embassy AtomicWaker wake
tails must deliver through the task route. Their safe std::task::Wake adapter
owns a valid Event; no raw executor task pointer or concurrent callback-field
registration is involved. Synthetic prestart/producer/unsupported overrides are
branch observations, not executions of a real RTOS scheduler or hardware IRQ.
