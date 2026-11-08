# Shared FreeRTOS Event validation

Run the registered gates from the repository root:

```sh
make test-freertos-event-waiters
make test-freertos-event-waiters-sanitize
make test-freertos-event-bindings
make test-freertos-event-waiters-arm
```

The host gates execute the actual FreeRTOS adapter and common owners against
initialized observers. Nested calls cover separated occurrences and retirement;
host threads cover concurrent waits, poll/timeout/abort removal, unrelated hints,
suspended-head behavior, coalescing and one successful consumer per occurrence.
The cleanup handshake waits until each unfinished observer has entered its next
relative wait before advancing the virtual clock.

The register observer covers the actual private wait step and task/ISR producer
bodies. It models fixed-threshold native entry, erratum `cpsie`, nested native
notifications and the trace task-tag getter. It checks intermediate effective
exclusion and exact mask, nesting and caller-mode restoration. Raw-mask wait-step
cases test the private operation; they do not admit masked callers to the public
wait API. ISR vectors install masks in a valid modeled handler and exclude
impossible entry through an interrupted task critical section. These are inert
models, including the 64-bit tick profiles; no FreeRTOS scheduler, blocking call,
exception delivery, trace buffer or hardware fault behavior is linked into them.

The binding gate shares the ordinary public C++, Rust and Zig Event APIs between
joined host workers in heap and zero-heap configurations. Test observer calls
coordinate workers; applications require no new API or FFI workaround. Storage
comes from the existing canonical C size probes.

The ARM gate runs real initialized native tasks on QEMU Cortex-M7, using ordinary
and MPU V1 ports, heap/zero-heap, optional native abort, and syscall enforcement.
Restricted tasks receive only their own stacks and the shared state region;
they receive no foreign waiter-stack grants. Linker wrappers observe original
mode and enabled masks before positive native blocks. IRQ0 calls the genuine
kernel-aware producer. IRQ1 runs above the syscall threshold and records only a
marker, exposing mask weakening without touching a kernel object. Mixed worker
priorities check yield aggregation. Failures are recorded before unconditional
safe release; workers are deleted only after the public wait returns and they
suspend. Reported high-water headroom is a bounded runtime observation.

The fixture generates the IRQ1 vector and native syscall section bounds in its
own build directory; SDK and board sources stay unchanged. V1 runtime uses the
workaround setting accepted by QEMU's CPU revision. The ordinary CM7 port runs
its stock unconditional erratum sequence. V1 workaround1 requires separate
model/source/object qualification; no assertion is suppressed to run it on an
unsupported virtual revision. Trace profiles execute the real marker/getter/ring
sources with privileged workers and the required state-statistics configuration.
They do not qualify restricted access to shared trace storage or SysTick.

Host profiles execute both ordinary and controlled-failure cases. The explicit
`OVE_FREERTOS_EVENT_WAITERS_CANARY`, `OVE_FREERTOS_EVENT_BINDINGS_CANARY`, and
`OVE_FREERTOS_EVENT_WAITERS_TARGET_CANARY` environment controls make their gate
fail while completing later observations. Every runner uses bounded process
ownership, checks full output and status, and records the ELF hash before each
execution. These gates do not establish physical IRQ latency, fault recovery,
whole-call-chain stack bounds, arbitrary MPU V2 support, or native 64-bit ticks.
