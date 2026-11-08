# Native queue contracts

Run `make test-queue-contracts` and `make test-queue-contracts-sanitize`.
Both gates cover heap and zero-heap configurations with QUEUE enabled and
SYNC, TIME and ASYNC disabled. Each gate validates 44 complete observations,
including deliberate early failures followed by successful controls. The CLI
reports one aggregate result. Logs preserve each executable's status and digest.

The four initialization observers compile the actual queue providers against
inert native functions. They check dimension rejection, staged initialization,
error propagation, reverse cleanup, unpublished handles and retry. They never
create an invalid kernel object or destroy a failed initialization. The NuttX
observer explicitly uses a semaphore count limit of seven and complete small
buffers. This is a boundary model, not an actual target limit or allocation
failure: the pinned NuttX `nxsem_init` always succeeds. Zephyr initialization
returns void and has no synthetic failure stage.

Queue-only context probes compile the actual IRQ providers. The additional
context models reuse the synchronization context observer and its existing
expected values, selecting Queue instead of SYNC. Architectural register reads
are explicitly substituted model inputs; these models do not claim hardware
interrupt or scheduler execution. Actual POSIX integration exercises allocation,
FIFO, full/empty results, teardown and backing reuse in both heap modes.

Set `OVE_QUEUE_CONTRACTS_CANARY=1` to inject an early failure into every native
observation. The controls still run, while native processes, the runner, CLI and
Make must fail. Clear the variable to restore the ordinary gate. Sanitizer runs
enable ASan, UBSan and leak detection without recovery.
