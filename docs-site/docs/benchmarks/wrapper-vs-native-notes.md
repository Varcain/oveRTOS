# Wrapper-vs-native notes

This page interprets the **wrapper-vs-native (within-run delta)**
section that appears in each per-RTOS benchmark report.  The raw
tables there pair every binding's wrapper measurement against its
raw native-API baseline measured in the same process; the prose below
explains what to read into the deltas, what to ignore as bench-design
or RTOS-shape artefacts, and what the structural cost of each
remaining gap actually is.

For raw numbers, see the per-RTOS reports:

- [FreeRTOS (heap)](freertos-heap.md), [FreeRTOS (zero-heap)](freertos-zeroheap.md)
- [NuttX (heap)](nuttx-heap.md), [NuttX (zero-heap)](nuttx-zeroheap.md)
- [Zephyr (heap)](zephyr-heap.md), [Zephyr (zero-heap)](zephyr-zeroheap.md)

All numbers below are taken from the current per-RTOS reports under
`CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` (caches and accelerators
off — see [benchmarks overview](index.md)).  Specific figures are
approximate and reflect the worst-case-timing patterns; the structural
argument is what's load-bearing, not the exact figures.  Re-check
against the current per-RTOS report if you're chasing a specific row.

## `native_*` rows in the main table

`bench_native_<rtos>.c` is C code calling raw RTOS APIs, compiled
identically into every binary.  The CPP / RUST / ZIG columns for those
rows are *the same C code measured in three different processes* —
they reflect cross-run scheduler noise, binary layout, and
linker-placement effects, not any binding-level behaviour.  The
meaningful number for binding overhead is the **within-run
wrapper-vs-native delta** in the dedicated section below each main
table.

## IPC caveats (per RTOS)

The wrapper-vs-native delta is a clean estimate of binding overhead
*only when the wrapper and the native baseline expose semantically
equivalent operations*.  For IPC primitives (queue, stream, event
group, workqueue) that's not always true; this section calls out
the gaps so the deltas don't get misread.

### POSIX

oveRTOS queue and stream are user-space ring buffers; the closest
standard POSIX IPC primitive is `pipe()`, which goes through the
kernel.  Large negative Δ on `Queue *` / `Stream *` rows therefore
reflects oveRTOS staying in user space, *not* a wrapper-layer effect.
Event groups and workqueues have no native POSIX equivalent and are
intentionally absent from the table.

### FreeRTOS

Both oveRTOS queue and the FreeRTOS `xQueue*` / `xStreamBuffer*`
baselines run in-kernel on Cortex-M, so wrapper-vs-native Δ on
`Queue *` / `Stream *` rows reflects pure binding overhead (no
user-space-vs-kernel asymmetry like on POSIX).  Event groups
(FreeRTOS `xEventGroup*` mirrors oveRTOS event 1:1, uninformative)
and workqueues (no FreeRTOS primitive) are intentionally absent.

### NuttX

NuttX's `Queue *` native baseline uses `mq_*` (POSIX message queue,
kernel-side via VFS path registration under `/var/mqueue/`), while
oveRTOS queue on NuttX is a user-space ring buffer over
`pthread_mutex+cond` — semantically narrower than `mq_*`.  Large
negative Δ on `Queue *` rows reflects that semantic asymmetry, not
wrapper magic.  `Stream *` rows have no native peer at all: oveRTOS
stream on NuttX is itself a user-space ring buffer over
`pthread_mutex+cond`, and NuttX has no kernel byte-stream primitive
that would be apples-to-apples (its closest, pipes, is a different
abstraction with VFS overhead).  Event groups and workqueues
likewise have no NuttX equivalent and are absent.

### Zephyr

Zephyr's `k_msgq` is the kernel message queue (semantically narrower
than the wrapper's user-space ring), and `k_pipe` is the kernel
byte-stream primitive (the closest analogue to oveRTOS stream).  Both
run in-kernel on Cortex-M, so wrapper-vs-native Δ on `Queue *` /
`Stream *` rows reflects the binding overhead rather than
user-space-vs-kernel asymmetry.  Event groups have no native peer
(Zephyr's `k_event` mirrors oveRTOS event 1:1, uninformative as a
comparison row); workqueues likewise.

## FreeRTOS lifecycle and intrinsic-cost gaps

Per-call wrapper overhead on FreeRTOS is in the 0–1 µs range under
worst-case timing: `thread/yield` ~30 ns, lock/unlock and take/give
~700 ns, recursive mutex ~800 ns, queue send+receive ~300 ns,
stream send+recv ~750 ns.  The wrapper functions themselves now
fetch from cold flash on every call, so the absolute gap is several
hundred nanoseconds bigger than under cache-on (where it was
typically ±200 ns); in *percent* terms the overhead remains under
10% of the corresponding native API call.

The remaining multi-µs gaps shown in the FreeRTOS wrapper-vs-native
table are **FreeRTOS-intrinsic costs of the underlying kernel work,
not wrapper-layer overhead**:

- **Thread create+destroy ≈ +10 µs (worst-case timing).**
  FreeRTOS task lifecycle (`prvInitialiseNewTask` +
  `prvAddNewTaskToReadyList` + `vTaskDelete` + list cleanup) runs
  regardless of how thin the wrapper is.  Under worst-case timing
  the kernel-side path itself is ~166 µs (was ~28 µs cache-on); the
  oveRTOS wrapper adds ~10 µs on top.  oveRTOS uses single-allocation
  static-task creation (no extra heap blocks) and task-notification
  join via a Dekker-style handshake (no separate semaphore object).
  Closing this further requires a thread-pool API (different
  ownership semantics) and is out of scope for the zero-overhead
  claim, which is about per-call cost.

- **Condvar signal+wait ≈ +14 µs (worst-case timing).**
  POSIX-style condvar contract requires the caller's mutex to be
  released atomically with the wait and re-acquired on wake.  That
  extra `xSemaphoreGive(mtx) + xSemaphoreTake(mtx)` round trip is
  what the raw `ulTaskNotifyTake` baseline doesn't pay.  The gap
  scales with the per-call mutex/sem cost (~7 µs each under
  worst-case timing), so a 2-call round trip is ~14 µs.  Anyone
  wanting condvar semantics pays this; the binding doesn't add to
  it.

- **Event signal+wait ≈ +30 µs (worst-case timing).**
  `ove_event_*` is implemented directly on top of FreeRTOS task
  notifications (no semaphore in the middle).  The residual gap vs
  raw `ulTaskNotifyTake` reflects the bench's bidirectional
  ping-pong shape: each iteration goes through the wrapper twice
  (signal + wait), and each half pays
  `xTaskGetCurrentTaskHandle` lookup + `evt->waiter` store + a
  context-switch worth of cold-flash fetches.  The wrapper buys
  uniform single-waiter semantics across RTOSes; the per-call cost
  is what it is.

- **Context switch (2t) ≈ +17 µs (worst-case timing).**  The bench
  runs a full ping-pong cycle (sem give/take across two tasks).
  Both directions go through the wrapper, so the wrapper cost adds
  twice; the native baseline pays the same context-switch cost
  without that doubling.

## Rust same-process native baseline (FreeRTOS) — resolved under worst-case timing

Earlier (cache-on) runs showed the Rust column for `native_*` rows
running +40–65% above the C column, despite `bench_native_freertos.c`
being *the same C code* compiled into every binary.  That elevation
turned out to be cache- and prefetch-driven: under
`CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` (the published
configuration) it converges.  Current Rust deltas on FreeRTOS heap
`native_*` rows:

| native row | Rust Δ vs C |
|---|---:|
| `native_mutex_lock_unlock` | +8.7% |
| `native_mutex_contention_2t` | +8.6% |
| `native_recursive_mutex_lock_unlock` | +3.7% |
| `native_sem_take_give` | +7.4% |
| `native_condvar_signal_wait` | −3.4% |
| `native_event_signal_wait` | −3.2% |
| `native_thread_yield` | −3.9% |
| `native_thread_context_switch` | −1.3% |
| `native_queue_send_receive` | +17.1% |

Most are within ±10%, max +17.1% on `native_queue_send_receive`.
The +40–65% same-process Rust elevation that prior cache-on runs
showed is **not present** under worst-case timing — meaning that
the cross-process Rust↔C variance was indeed driven by binary-layout
interaction with the I-cache and ART accelerator.  With those
disabled, the Rust and C process images converge to within ordinary
scheduler noise.  No further binding-side action required; the
worst-case-timing configuration that the published numbers run in
is also what makes the same-process baseline correct.

Note: `native_thread_create_destroy` Rust delta is −78.3% — this
is an unrelated effect (Rust's `Thread::spawn+join` uses static
stack memory rather than going through the kernel pool, just like
the C++ `Thread (ctor+dtor)` path).  Not a same-process baseline
issue; documented in the [per-binding analysis](per-binding.md).

## `mutex_contention_2t` cross-binding and run-to-run flakiness

With `CONFIG_TIMESLICING=y`, Zephyr round-robins same-priority
threads every 1 ms.  Whether the bench's contention helper actually
collides with the runner during the measurement window depends on
initial scheduling alignment — some bindings may show ~22 µs (real
contention happened) while others in the same run show ~3 µs
(helper ran in a different time slice from the runner, no
collision).  The wrapper code path is identical across bindings, so
cross-binding variance on this row is bench-design noise, not
binding overhead.

FreeRTOS and NuttX schedulers don't preempt same-priority threads,
so the helper typically never runs at all and the row reads
"no contention" values (~5–10 µs cache-off, vs the ~25–50 µs that
real contention would produce).  Even there, however, this row
exhibits noticeable **run-to-run drift** under worst-case timing on
FreeRTOS — successive runs of the same binary on the same board
have been observed to swing 25–55% on this case (e.g. C:
8.2 µs → 10.2 µs; Rust: 8.9 µs → 13.7 µs across two adjacent runs).
Both numbers are still in the "no real contention" range, but the
relative gap is large enough to dominate any run-to-run instability
budget on FreeRTOS.  Per-call sync rows (lock/unlock, take/give,
signal/wait) are stable to <2% across runs; this case alone is the
exception.

The takeaway: don't read fine-grained cross-binding signal off the
`mutex_contention_2t` row on any RTOS — Zephyr swings on
timeslice-alignment, FreeRTOS swings on scheduling-jitter alignment,
NuttX is most stable but the helper never collides anyway.
