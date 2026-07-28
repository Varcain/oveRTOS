# LXP Complexity Remediation Baseline

This is the frozen Iteration 0 baseline for the structural remediation plan.
It records the last hardware measurements before the remediation guardrails
were added. Later iterations must compare against these numbers and explain
changes outside normal run-to-run variance.

## Revisions and configuration

The architecture review and Iteration 0 implementation started on 2026-07-26
at:

- oveRTOS `92969a85f567`;
- LXP `a719b6c0dbfd`;
- Buildroot NOMMU BusyBox/Hush with uClibc-ng, hard-float ARM FDPIC userspace;
- STM32F746G-DISCO, 216 MHz, ST-LINK VCP console;
- 10 ms guest quantum, QSPI rootfs, Ethernet, PTY, framebuffer, DMA2D, touch,
  and RT-scope enabled;
- network filesystem and the separate LXP latency profile disabled.

The generated `linux_interop` configuration hashes were:

| Engine | `.config` SHA-256 | `NREG` | `NSLOT` |
|---|---|---:|---:|
| FreeRTOS | `99b72157357edea92981542412fa6db5913eaea2374622c195b3493955abbe10` | 11 | 15 |
| NuttX | `86b9d1206831b7d0f1ca66e71b6e3983637fefd552964b9b4b1d51fbb3092ce9` | 11 | 15 |
| Zephyr | `b77d04e8424393d30c9d0b1d3e97082e176931fce20fe3817f4bc6db0765ed18` | 12 | 16 |

Each region has a 131,072-byte program area and a 524,288-byte dynamic pool.
The generated configurations live under
`output/stm32f746g-discovery/<engine>/linux_interop/.config`.

## Firmware and static RAM

These are the pre-guardrail `linux_interop` images used as the size baseline.
The FreeRTOS image is the build-tree ELF, not the stale copy under `images/`.
Flash and physical-memory figures come from each engine's linker report.

| Engine image | Build identity | Flash used | Initialized data | Internal static RAM | External static/reserved RAM |
|---|---|---:|---:|---:|---:|
| FreeRTOS | `ove-753e056 lxp-a719b6c` | 224,628 B | 1,272 B | 251,720 B | 7,738,576 B |
| NuttX | `ove-92969a8 lxp-a719b6c` | 226,484 B | 1,996 B | 239,892 B | 7,506,440 B minimum |
| Zephyr | `ove-499b386 lxp-a719b6c` | 273,756 B | 12,924 B | 258,304 B | 8,303,680 B |

FreeRTOS internal RAM is the reported 46,624-byte DSP/DTCM span, 204,776-byte
RAM span, and 320-byte Ethernet descriptor span. Its external figure includes
the 7,737,040-byte SDRAM span and 1,536-byte Ethernet TX span. Zephyr reports
240 KiB RAM plus 12,544 bytes DTCM.

The NuttX linker cannot account for fixed-address SDRAM pointers. Its external
minimum is therefore calculated explicitly: 7,208,960 bytes of guest pools,
261,120 bytes of framebuffer, 21,000 bytes of per-slot exec capture, and
15,360 bytes of trusted native-task stacks. It excludes address-alignment gaps.

The Iteration 0 guardrail build has the following cost relative to those
images. The configurations and root filesystem are unchanged.

| Engine | Flash used | Flash delta | Internal static RAM | RAM delta |
|---|---:|---:|---:|---:|
| FreeRTOS | 228,620 B | +3,992 B | 252,232 B | +512 B |
| NuttX | 231,996 B | +5,512 B | 239,988 B | +96 B |
| Zephyr | 277,044 B | +3,288 B | 258,304 B | 0 B |

The RAM delta is the linker-region delta, not an estimate of the diagnostic
objects. Packing and alignment account for the engine-to-engine difference.
The diagnostic state itself is one 72-byte health record, one byte of census
state, and one native-task-presence byte per slot. Snapshot and validation
records are caller-owned stack objects.

The target-ABI size report compiled into these images records:

| Object | FreeRTOS | NuttX | Zephyr |
|---|---:|---:|---:|
| `lxp_proc_t` | 416 B | 416 B | 416 B |
| `lxp_mm_t` | 68 B | 68 B | 68 B |
| `lxp_files_t` | 130 B | 130 B | 130 B |
| `lxp_fs_context_t` | 260 B | 260 B | 260 B |
| `lxp_sighand_t` | 268 B | 268 B | 268 B |
| `lxp_thread_group_t` | 104 B | 104 B | 104 B |
| `lxp_arena_t` | 276 B | 276 B | 276 B |
| Per-slot exec capture | 1,400 B | 1,400 B | 1,400 B |
| Per-slot resume context | 200 B | 200 B | 200 B |
| Per-slot coordinator core | 1,324 B | 1,324 B | 1,324 B |
| Per-region coordinator core | 286 B | 286 B | 286 B |
| Process slot table | 6,240 B | 6,240 B | 6,656 B |
| Reported coordinator static set | 23,232 B | 23,232 B | 24,852 B |

The “coordinator core” totals intentionally cover the current parallel
lifecycle, mailbox, context, signal-save, snapshot-arena, and diagnostic
tables. Later iterations must reduce or explain changes to this exact set.

## Ten-minute userspace and network workload

The workload is the regular input-driven `lvmusic` demo after an automated Play
tap, plus a loop repeatedly downloading a 20 MiB object over HTTP from
`172.1.1.1`. Each measured interval is 600 seconds. Raw console, top, and JSON
summaries are under
`output/lvmusic-net-comparison-20260726-post-audit/`.

The runtime images all used LXP `a719b6c`. Their engine-specific oveRTOS
identities were FreeRTOS `753e056`, NuttX `0c63244`, and Zephyr `499b386`.
The later NuttX `92969a8` change affects CPU attribution only and is represented
in the size image above, not this runtime capture.

| Metric | FreeRTOS 11.2.0 | NuttX 12.12.0 | Zephyr 4.4.0 |
|---|---:|---:|---:|
| Active render samples | 1,771 | 1,797 | 1,781 |
| FPS median / mean / p95 | 6 / 6.06 / 7 | 6 / 6.01 / 6 | 6 / 6.13 / 7 |
| Render ms median / mean / p99 / max | 140 / 140.68 / 166 / 368 | 137 / 137.09 / 154 / 244 | 129 / 128.77 / 138 / 145 |
| Flush ms median / p99 / max | 5 / 6 / 40 | 5 / 6 / 13 | 5 / 7 / 17 |
| LVGL CPU mean | 99.99% | 100% | 100% |
| Successful 20 MiB downloads | 5 | 5 | 7 |
| Network failures | 0 | 0 | 0 |
| Successes/minute | 0.475 | 0.475 | 0.664 |
| Serial reconnects | 0 | 0 | 0 |

At the login shell, FreeRTOS and NuttX had 11/15 slots and 7/11 regions free.
Zephyr had 12/16 slots and 8/12 regions free. The corresponding effective
guest capacity was 4,587,520 bytes for FreeRTOS/NuttX and 5,242,880 bytes for
Zephyr.

## Real-time and SVC measurements

| Metric | FreeRTOS | NuttX | Zephyr |
|---|---:|---:|---:|
| Accepted releases / executions | 545,386 / 545,386 | 592,960 / 592,960 | reporter prefix lost |
| Missed / late-finish / IRQ overrun | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 in accumulated line |
| Pending max | 0 | 0 | 0 |
| Dispatch min / weighted avg / max | 5.89 / 9.50 / 71.43 us | 6.46 / 12.22 / 221.72 us | 5.78 / 9.33 / 59.17 us accumulated |
| Worst p99 / p99.9 upper bucket | 16 / 32 us | 20 / 32 us | 32 / 32 us accumulated |
| Work min / max | 5.07 / 9.52 us | 4.96 / 10.07 us | 4.94 / 9.83 us accumulated |
| SVC calls | 296,923 accepted-window calls | not instrumented | 436,087 accumulated-window calls |
| SVC weighted avg / max | 13.97 / 15.76 us | not instrumented | 12.59 / 14.86 us |
| Longest SVC | `exit_group(248)` | not instrumented | `exit_group(248)` |

Two limitations are part of the baseline rather than silently filled with
estimates:

- NuttX did not provide SVC timing in this image.
- Under the Zephyr render load the low-priority UART reporter could not emit
  periodic records. Its first post-load aggregate retained dispatch, work, SVC,
  miss, late, overrun, and pending values, but the interleaved shell output
  overwrote the release/execution prefix. The JSON parser therefore reports
  zero accepted windows. The table uses only the intact aggregate fields.

## Iteration 0 guardrail

The baseline is paired with the target-ABI `lxp_diag_size_report()`, structured
slot/region snapshots, and read-only `lxp_validate_world()`. The
`linux_interop` teardown audit prints the exact compiled object sizes and the
number of automatic validator checks/failures. The validator checkpoints after
launch, after each existing coordinator-statistics refresh, and after teardown.
It neither repairs state nor allocates memory.

## Iteration 1 explicit child construction

Iteration 1 removes the whole-`lxp_proc_t` assignment from fork and clone.
Process-child and thread-child constructors now acquire only the address-space,
file-table, filesystem-context, signal-handler, and thread-group objects
selected by the clone flags. Task-local coordinator, wait, signal-delivery,
timer, and request state starts empty instead of being inherited and repaired
afterward.

Child preparation is a transaction: it acquires the region reference, process
resources, native mapping, and optional vfork snapshot before publishing the
slot. Every failed stage unwinds through the same reverse-order cleanup path.
The table-driven host test covers all 16 valid combinations of `CLONE_VM`,
`CLONE_FILES`, `CLONE_FS`, `CLONE_SIGHAND`, and `CLONE_THREAD`. Separate fault
tests exhaust the reference count at each resource acquisition and force
region, native-map, and snapshot failures; each verifies restored references
and a clean world invariant.

The target ABI, generated configurations, root filesystem, slot and region
counts, coordinator static tables, and internal/external static RAM are
unchanged. The production `linux_interop` images compare with the Iteration 0
guardrail as follows:

| Engine | Iteration 0 flash | Iteration 1 flash | Flash delta | Relative delta | Internal static RAM |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 228,620 B | 229,004 B | +384 B | +0.168% | 252,232 B |
| NuttX | 231,996 B | 232,404 B | +408 B | +0.176% | 239,988 B |
| Zephyr | 277,044 B | 277,820 B | +776 B | +0.280% | 258,304 B |

All 44 host CTest targets pass. The coordinator suite now runs 61 tests, up
from 58, and the unit and coordinator suites also pass under AddressSanitizer
and UndefinedBehaviorSanitizer. On the STM32F746G-DISCO, the NuttX image
completed the RTOS/Linux round trip and the BusyBox init sequence through
`rcS`, getty, and inetd (PID 9). A same-board Iteration 0 A/B image reproduced
the current serial `root/root` rejection against byte-identical QSPI
`/etc/shadow` data, so that credential-path issue is not an Iteration 1
regression.

## Iteration 2 typed coordinator state

Iteration 2 replaces the independent lifecycle request flags and wait flags in
`lxp_proc_t` with one tagged intent record and one tagged wait record. The
intent kinds are `NONE`, `FORK`, `EXEC`, and `EXIT`; the wait kinds are
`NONE`, `SLEEP`, `WAITPID`, `READ`, `WRITE`, `POLL`, `SELECT`, `SOCKET`, and
`DEVICE`. Each record owns the payload associated with its current kind, so a
slot can no longer simultaneously represent two incompatible requests or
retain stale payload from an earlier request.

All state transitions go through `lxp_intent_*()` and `lxp_wait_*()` helpers.
They reject conflicting starts, require completion to name the expected kind,
clear the complete tagged record, and diagnose invalid transitions. Exit is
the deliberate exception to normal exclusivity: it supersedes lower-priority
deferred work so fatal signals and RTOS memory-fault seams cannot strand a
process behind a pending syscall. Generic signal interruption now handles
every wait kind, including device waits. The primary-event bitmap remains a
scheduling hint; event claiming validates the tagged kind before dispatch.

The target-ABI reduction is:

| Object | Iteration 1 | Iteration 2 | Delta |
|---|---:|---:|---:|
| `lxp_proc_t` | 416 B | 232 B | -184 B (-44.2%) |
| Per-slot coordinator core | 1,324 B | 1,140 B | -184 B (-13.9%) |
| FreeRTOS/NuttX process slot table | 6,240 B | 3,480 B | -2,760 B (-44.2%) |
| Zephyr process slot table | 6,656 B | 3,712 B | -2,944 B (-44.2%) |
| FreeRTOS/NuttX coordinator static set | 23,232 B | 20,472 B | -2,760 B (-11.9%) |
| Zephyr coordinator static set | 24,852 B | 21,908 B | -2,944 B (-11.8%) |

Clean production images built from oveRTOS `8dcc757` and LXP `015529b`
compare with Iteration 1 as follows:

| Engine | Iteration 1 flash | Iteration 2 flash | Flash delta | Internal static RAM | RAM delta |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 229,004 B | 229,812 B | +808 B (+0.353%) | 249,160 B | -3,072 B (-1.22%) |
| NuttX | 232,404 B | 233,156 B | +752 B (+0.324%) | 237,204 B | -2,784 B (-1.16%) |
| Zephyr | 277,820 B | 276,920 B | -900 B (-0.324%) | 254,208 B | -4,096 B (-1.59%) |

The engine configurations, root filesystem, slot and region counts, and
external-memory allocations are unchanged. Their generated `.config` hashes
remain the values recorded at the top of this document. The engine-dependent
linker deltas differ slightly from the slot-table reductions because of
section placement and alignment.

All 44 host CTest targets pass. The coordinator suite now runs 64 tests,
including exclusive intent transitions, exit supersession, every tagged wait
transition, and the complete wait-to-event mapping. The unit and coordinator
suites also pass under AddressSanitizer and UndefinedBehaviorSanitizer. The
ordinary FreeRTOS QEMU suite passes; the separate `linux-segv` QEMU target
could not start because its configured rootfs load range overlaps a firmware
ELF segment, before any guest code executes.

FreeRTOS, NuttX, and Zephyr images were each exercised on the
STM32F746G-DISCO. All three completed the RTOS/Linux round trips and reached
BusyBox `rcS`, getty, and inetd with zero RT-scope misses. A clean Iteration 1
FreeRTOS A/B image reproduced the occasional cold-boot `vfork`/exec startup
errors seen during validation, while a subsequent Iteration 2 boot completed
cleanly. The behavior is therefore retained as a pre-existing startup issue,
not attributed to the typed-state change.

## Iteration 3 consolidated slot lifetime

Iteration 3 makes one private `lxp_slot_runtime` the core authority for a slot
incarnation. It contains the process record (and therefore the typed intent and
wait state from Iteration 2), resume context, deferred mailbox, generation,
host lifecycle, and runnable publication. This replaces the independently
indexed process, resume, mailbox, generation, lifecycle, and used/runnable
arrays. At this point public process-table access became a bounded read
operation rather than a writable global. A later descriptor-ownership pass
removed even that transitional accessor: pipe and PTY endpoint liveness follows
open-file-description lifetime, PTY signals route through a narrow
process-group operation, and `sysinfo` consumes an aggregate process count.

`lxp_slot_ref_t` carries a slot index and generation across dispatch, fault,
transaction, and delayed-completion boundaries. `lxp_region_ref_t` does the
same for an address-space region. A prepared or snapshot region starts with a
temporary slot-generation lease; successful construction transfers ownership
to the `lxp_mm_t` address-space capability and clears that lease. Consequently,
`CLONE_VM` tasks can outlive the slot that first acquired the region, and the
region is released only after the final shared address-space reference.

The three RTOS seams retain their native task handles and generation mirrors,
but can no longer modify LXP process or runnable state. They use narrow
generation-checked operations for runnable queries, syscall dispatch, region
lookup, and memory-fault publication. The old writable process/used globals
and public bare-slot dispatch/event entry points are gone.

The target-ABI structural change relative to Iteration 2 is:

| Object | Iteration 2 | Iteration 3 | Delta |
|---|---:|---:|---:|
| `lxp_proc_t` | 232 B | 240 B | +8 B |
| `lxp_mm_t` | 68 B | 72 B | +4 B |
| Aggregated slot runtime | parallel arrays | 464 B | single authority |
| Deferred request | parallel 16 B entry | 16 B embedded | 0 B payload |
| Per-slot coordinator core | 1,140 B | 1,160 B | +20 B |
| Per-region coordinator core | 286 B | 292 B | +6 B |
| FreeRTOS/NuttX coordinator static set | 20,472 B | 20,703 B | +231 B (+1.13%) |
| Zephyr coordinator static set | 21,908 B | 22,156 B | +248 B (+1.13%) |

The small increase buys generation-bearing capabilities in every stored slot
and address-space reference. The 6,960-byte FreeRTOS/NuttX slot-runtime table
is not directly comparable with Iteration 2's 3,480-byte process-only table:
it now also contains the former resume, deferred, generation, lifecycle, and
runnable arrays. The complete coordinator-static totals above are the
like-for-like comparison.

Clean production images built from oveRTOS `348a54d` and LXP `a7f3c4a`
compare with Iteration 2 as follows:

| Engine | Iteration 2 flash | Iteration 3 flash | Flash delta | Internal static RAM | RAM delta |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 229,812 B | 232,204 B | +2,392 B (+1.04%) | 249,672 B | +512 B |
| NuttX | 233,156 B | 236,916 B | +3,760 B (+1.61%) | 237,524 B | +320 B |
| Zephyr | 276,920 B | 279,352 B | +2,432 B (+0.88%) | 258,304 B | +4,096 B |

The generated configurations, root filesystem, slot and region counts, and
external-memory reservations remain unchanged; the three `.config` hashes
still match the values at the top of this document. Zephyr's 4 KiB RAM delta
is a linker/MPU-region allocation step, not 4 KiB of new coordinator objects;
the measured Zephyr coordinator-static increase is 248 bytes.

All 44 host CTest targets pass in normal and AddressSanitizer plus
UndefinedBehaviorSanitizer builds. The coordinator suite now runs 69 tests,
including stale slot and region capabilities, generation wrap that skips zero,
stale memory-fault publication, lease transfer, and shared-address-space
release after the final task reference. The Cortex-M4 QEMU suite reports
`PASS: lxp-m4-ok`.

FreeRTOS, NuttX, and Zephyr images were each flashed to the
STM32F746G-DISCO. Every engine completed the three RTOS/Linux phase-one
round trips, reached BusyBox `rcS`, getty, and inetd, and reported zero
RT-scope misses. The first complete RT-scope windows observed maximum dispatch
latencies of 25.11 us on FreeRTOS, 24.33 us on NuttX, and 55.09 us on Zephyr.

## Iteration 4 decomposed coordinator

Iteration 4 separates coordinator policy into private lifecycle, guest-event,
primary-event, blocked-operation, fork/vfork, exec, and exit/reap modules.
The initial split kept them as unity-included implementation parts. A later
complexity pass converted all seven to compiled translation units behind
`src/run/lxp_coordinator.h`. The slot and region tables remain private to
`lxp_run.c`; policy modules can mutate them only through the private
coordinator contract. Standalone LXP, oveRTOS engine builds, coordinator tests,
and the Cortex-M7 QEMU harness now compile the same module boundaries. Stub and
fuzz targets continue to exclude the coordinator as a whole.

The main coordinator loop now performs only event claim, typed primary
dispatch, typed blocked-state scan, liveness/maintenance, and event wait. Its
fair rotating cursor, one-event claim limit, bounded critical section,
readiness wakeups, nearest-deadline calculation, and existing polling fallback
are unchanged. The source is split by ownership rather than textual inclusion;
the compiler now rejects undeclared cross-policy dependencies.

Handlers express lifecycle changes as typed outcome requests through one
applicator. Only the lifecycle module invokes `spawn_launch`, `spawn_resume`,
`park_prepare`, `park_slot`, or `abort_slot`; it also owns the corresponding
host-state and runnable publication. A failed native transition therefore
follows the same containment path regardless of whether it originated in fork,
exec, exit, signal, timeout, or an I/O retry.

The seam now distinguishes starting a captured fork child from resuming a
parked native task with an explicit `spawn_resume` mode. The coordinator
publishes the generation-qualified runnable capability before a port can
schedule either task, and ports publish their matching native generation before
calling an RTOS API that can dispatch it. Resume behavior no longer depends on
inferring intent from a mutable runnable bit or native handle.

Clean production images built from oveRTOS `75113a4` and LXP `5adf7fc`
compare with Iteration 3 as follows:

| Engine | Iteration 3 flash | Iteration 4 flash | Flash delta | Internal static RAM | RAM delta |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 232,204 B | 233,052 B | +848 B (+0.365%) | 249,672 B | 0 B |
| NuttX | 236,916 B | 239,012 B | +2,096 B (+0.885%) | 237,524 B | 0 B |
| Zephyr | 279,352 B | 279,808 B | +456 B (+0.163%) | 258,304 B | 0 B |

The additional flash is executable transition policy exposed as function
boundaries rather than new runtime state. Target ABI sizes, coordinator static
tables, external-memory reservations, root filesystem, slot and region counts,
and generated configurations are unchanged. All three `.config` hashes still
match the values recorded at the top of this document.

All 44 normal host test targets pass. The coordinator suite now runs 73 tests,
including fair cursor rotation, stale-hint consumption, primary park outcome,
expired-timer resume, and fail-closed rejection of an out-of-range claimed
slot. All 73 coordinator tests also pass under AddressSanitizer and
UndefinedBehaviorSanitizer. Seven ARM feature-gate combinations build
warning-clean, the Cortex-M4 QEMU suite reports `PASS: lxp-m4-ok`, and the
decoupling check passes. The syscall documentation gate still reports the
pre-existing unclassified `link`/`linkat` matrix entries; the coordinator
change does not alter syscall classification.

FreeRTOS, NuttX, and Zephyr images built from oveRTOS `a92b03b` and LXP
`5adf7fc` were each flashed to the STM32F746G-DISCO. Every engine reported the
expected clean build identity, completed all three RTOS/Linux round trips,
reached BusyBox `rcS`, getty, and inetd, and produced consecutive RT-scope
windows with zero misses, late finishes, IRQ overruns, or pending executions.

| Engine | Startup releases | Startup p99 / p99.9 / max | Next-window releases | Next-window p99 / p99.9 / max |
|---|---:|---:|---:|---:|
| FreeRTOS | 10,030 | <=10 / <=12 / 23.24 us | 10,045 | <=8 / <=8 / 8.28 us |
| NuttX | 10,120 | <=12 / <=16 / 24.13 us | 10,030 | <=10 / <=10 / 9.87 us |
| Zephyr | 10,016 | <=10 / <=12 / 49.13 us | 10,049 | <=10 / <=12 / 36.56 us |

FreeRTOS and Zephyr also completed the early host socket smoke. NuttX brought
up Ethernet and later started the Linux network services, but retained its
known early-boot socket-smoke `connect failed rc=-9` result. The coordinator
decomposition therefore introduces no observed readiness, scheduling, or
real-time regression.

## Iteration 5 transactional lifecycle

Iteration 5 gives fork/vfork and exec explicit transaction records instead of
spreading ownership transfer across success and error branches. A `fork_txn`
owns the destination generation, parent-region reference, child process
objects, native mappings, child accounting, and optional vfork snapshot until
publication commits them. An `image_txn` constructs an FDPIC image off-slot,
then moves the completed process record into the slot before starting its
native task and transferring the region lease to the address space. An
`exec_txn` reserves and validates the replacement first, preserves the old
image and process objects until the commit boundary, and detaches them only
after the old native task has stopped.

Every transaction has one phase-aware, idempotent abort path. Before exec
commit, abort releases only the new reservation and resumes the unchanged old
image. After commit, failure contains the transitioning guest, stops any
published native task before releasing its resources, resumes a restored
vfork parent exactly once, and leaves unrelated slots untouched. If the RTOS
cannot confirm that a task stopped, its Linux resources remain attached rather
than becoming dangling references. A stale vfork snapshot capability is
treated as ownership corruption: the snapshot is never copied, and the
affected parent/child pair is contained instead of leaving the parent parked.

Thirteen single-shot, test-only failpoints cover every fork and exec
acquisition, validation, publication, native-start, and region-commit
boundary. Production builds compile the injection control surface out. Each
injected failure runs the world validator after abort, including a second
abort call, and checks resource identity before commit or isolated guest
containment after commit.

Clean production images built from oveRTOS `f03a724` and LXP `116660e`
compare with Iteration 4 as follows:

| Engine | Iteration 4 flash | Iteration 5 flash | Flash delta | Internal static RAM | RAM delta |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 233,052 B | 235,116 B | +2,064 B (+0.886%) | 249,672 B | 0 B |
| NuttX | 239,012 B | 241,252 B | +2,240 B (+0.937%) | 237,524 B | 0 B |
| Zephyr | 279,808 B | 281,480 B | +1,672 B (+0.598%) | 258,304 B | 0 B |

The added flash is transaction and containment policy; transactions are
coordinator-stack objects and add no static tables. Target ABI sizes, slot and
region counts, external-memory reservations, root filesystem, and generated
configurations are unchanged. The FreeRTOS, NuttX, and Zephyr `.config`
SHA-256 values still match those recorded at the top of this document.

All 44 host CTest targets pass in both normal and AddressSanitizer plus
UndefinedBehaviorSanitizer builds. The coordinator suite now runs 78 tests,
up from 73, with real coverage of all 13 failpoints plus stale-snapshot
containment. Seven ARM feature-gate combinations build warning-clean, the
decoupling guard passes, and the Cortex-M4 QEMU suite reports
`PASS: lxp-m4-ok`.

FreeRTOS, NuttX, and Zephyr were each flashed to the STM32F746G-DISCO with the
clean `ove-f03a724 lxp-116660e` identity. Every engine completed the three
RTOS/Linux phase-one round trips, reached BusyBox `rcS`, getty, and inetd, and
then completed an SSH `uname`, external `/bin/echo`, and three shell-loop
fork/exec iterations. Every observed RT-scope window had zero misses, late
finishes, IRQ overruns, and pending executions.

| Engine | Startup releases | Startup p99 / p99.9 / max | Next-window releases | Next-window p99 / p99.9 / max |
|---|---:|---:|---:|---:|
| FreeRTOS | 10,030 | <=10 / <=12 / 22.76 us | 10,046 | <=8 / <=8 / 7.85 us |
| NuttX | 10,120 | <=12 / <=16 / 23.52 us | 10,030 | <=10 / <=10 / 9.83 us |
| Zephyr | 10,016 | <=10 / <=20 / 66.54 us | 10,049 | <=10 / <=12 / 13.28 us |

FreeRTOS and Zephyr again completed the early host socket smoke. NuttX
retained its known early `connect failed rc=-9` result but subsequently brought
up inetd and completed the SSH lifecycle test. The board was left running the
clean NuttX image.

## Iteration 6 dispatch-scoped guest memory

Iteration 6 replaces distributed privileged guest-pointer handling with one
common boundary. `lxp_guest_range_ok()` is the pure range and permission
primitive. Dispatch-scoped views bind a process, address space, slot
generation, and region generation for the duration of one SVC or coordinator
dispatch. Copy, scalar, and bounded-string helpers revalidate that identity
before access; stale, nested, or permission-incompatible views fail closed.
View teardown is idempotent, and a diagnostic records any view which survives
its dispatch.

Syscall metadata, strings, vectors, signal frames, and blocked-operation
payloads now pass through that boundary, as do the network, PTY, netfs, and
device bridges. Blocked state retains guest addresses rather than privileged
CPU pointers. Netfs completion is marshalled only while the owning guest's
coordinator view is active. A repository check rejects new direct guest
validation or copies outside the common implementation.

Hardware measurement exposed full 32-byte clears at both ends of every view
lifecycle. The final hot-path commit makes `active` the revocation point,
clears only authority-bearing pointers at teardown, and initializes every
field directly. ARM disassembly confirms that neither begin nor end calls
`memset()`. In the same boot-plus-fork workload, this reduced the FreeRTOS SVC
lifetime average/maximum from 17.00/19.04 us to 15.28/17.22 us. Zephyr's final
figures are 13.86/16.23 us, down from 16.15/18.29 us before the cleanup.

Clean production images built from oveRTOS `0418a4a` and LXP `bedb54b`
compare with Iteration 5 as follows:

| Engine | Iteration 5 flash | Iteration 6 flash | Flash delta | Internal static RAM | RAM delta |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 235,116 B | 236,300 B | +1,184 B (+0.504%) | 249,672 B | 0 B |
| NuttX | 241,252 B | 241,540 B | +288 B (+0.119%) | 237,524 B | 0 B |
| Zephyr | 281,480 B | 282,560 B | +1,080 B (+0.384%) | 258,304 B | 0 B |

The added flash is the common access and generation-validation policy.
`lxp_guest_view_t` is a 32-byte dispatch-local object and adds no static
table. Target ABI sizes remain 240 bytes for `lxp_proc_t` and 72 bytes for
`lxp_mm_t`; existing alignment absorbed the process view pointer. Slot and
region counts, internal and external static RAM, root filesystem, and
generated configurations are unchanged. The three `.config` SHA-256 values
still match those recorded at the top of this document.

All 45 host CTest targets pass in both normal and AddressSanitizer plus
UndefinedBehaviorSanitizer builds. The coordinator suite still runs 78 tests.
All six fuzz replay corpora pass under the sanitizers, seven ARM feature-gate
combinations build warning-clean, and both the decoupling and guest-memory
repository guards pass. The Cortex-M4 QEMU suite reports
`PASS: lxp-m4-ok`.

FreeRTOS, NuttX, and Zephyr were each flashed with the clean
`ove-0418a4a lxp-bedb54b` identity. Every engine completed the three
RTOS/Linux phase-one round trips, BusyBox `rcS`, getty, inetd, serial login,
`uname`, and three external `/bin/true` executions. An independent SSH session
through the Pi gateway repeated `uname` and the three external executions on
each engine. FreeRTOS and Zephyr completed the early socket smoke; NuttX
retained its known early `connect failed rc=-9` result and then completed the
same serial and SSH lifecycle checks.

Every captured RT-scope window had zero misses, late finishes, IRQ overruns,
and pending executions:

| Engine | Startup releases | Startup p99 / p99.9 / max | Next-window releases | Next-window p99 / p99.9 / max |
|---|---:|---:|---:|---:|
| FreeRTOS | 10,030 | <=10 / <=16 / 24.52 us | 10,047 | <=10 / <=10 / 23.59 us |
| NuttX | 10,120 | <=12 / <=20 / 28.20 us | 10,030 | <=10 / <=16 / 17.91 us |
| Zephyr | 10,016 | <=10 / <=32 / 68.22 us | 10,048 | <=10 / <=12 / 35.15 us |

NuttX does not expose the SVC timer. FreeRTOS's first-window SVC
average/maximum was 15.19/17.22 us across 1,029 calls; Zephyr's was
13.68/16.23 us across 970 calls. Against the frozen loaded-workload baseline,
those averages increased by 8.7% and maxima by 9.3%/9.2%, while the serial and
SSH syscall workloads completed without capacity, timeout, or real-time
failures. The board was left running the clean FreeRTOS image.

## Iteration 7 explicit cache and MPU contracts

Iteration 7 makes the CPU-memory model an engine declaration rather than an
assumption hidden in cache hooks and MPU constants. `lxp_run()` validates the
live cache state after engine preparation and before loading a guest. FreeRTOS,
NuttX, and Zephyr declare a coherent single-CPU model in which the privileged
coordinator and guest use matching cacheable Normal-memory attributes for
ordinary program and dynamic-pool memory. A mismatch fails launch closed.

The coordinator now produces one immutable `lxp_memory_policy_t`. Its reusable
key contains the slot generation, address-space region and generation,
device-map generation, and copied-text execute-policy generation. Each engine
compiles that policy into its native representation: restricted task regions
on FreeRTOS, RBAR/RASR profiles on NuttX, and memory-domain partitions on
Zephyr. Native state is reused only when the complete key matches. Device
mappings still originate in registered driver capabilities; the policy does
not accept a userspace-selected physical range.

Framebuffer and DMA2D mappings remain outside the ordinary CPU-memory model.
They are device capabilities with their own attributes and explicit ownership
boundaries. The STM32 framebuffer paths retain their clean/invalidate
operations around DMA ownership transfer. The general guest-memory path does
not acquire a mixed cached/uncached alias.

The target-ABI changes relative to Iteration 6 are:

| Object | Iteration 6 | Iteration 7 | Delta |
|---|---:|---:|---:|
| `lxp_proc_t` | 240 B | 240 B | 0 B |
| `lxp_mm_t` | 72 B | 84 B | +12 B |
| `lxp_memory_policy_t` | absent | 60 B | dispatch-local |
| `lxp_memory_policy_key_t` | absent | 28 B | native prepared-state key |
| `lxp_os_ops_t` | 116 B | 124 B | +8 B |

Clean production images culminate at oveRTOS `bdd37f4` and LXP `6cfad27`.
They compare with Iteration 6 as follows:

| Engine | Iteration 6 flash | Iteration 7 flash | Flash delta | Iteration 6 RAM | Iteration 7 RAM | RAM delta |
|---|---:|---:|---:|---:|---:|---:|
| FreeRTOS | 236,300 B | 237,028 B | +728 B (+0.308%) | 249,672 B | 250,376 B | +704 B |
| NuttX | 241,540 B | 242,632 B | +1,092 B (+0.452%) | 237,524 B | 238,740 B | +1,216 B |
| Zephyr | 282,560 B | 283,496 B | +936 B (+0.331%) | 258,304 B | 258,304 B | 0 B |

The generated configurations, root filesystem, slot and region counts, and
external-memory reservations are unchanged. Their three `.config` hashes still
match the values at the top of this document. The relevant target declarations
are FreeRTOS `TEX_S_C_B_SRAM=0x0b`, NuttX D-cache enabled with write-back,
write-allocate Normal memory, and Zephyr `CONFIG_CACHE_MANAGEMENT=y` plus
`CONFIG_DCACHE=y`.

NuttX hardware stress exposed one latency problem in the original launch
publication path. It flushed the complete 128 KiB program region and 256 KiB
dynamic pool before every launch. NuttX converts a clean range at least as
large as the Cortex-M7's 16 KiB D-cache into a whole-cache set/way clean plus a
barrier. Draining dirty SDRAM lines at that barrier could postpone exception
entry across multiple 1 ms releases. Matching WBWA CPU mappings require no
launch-time maintenance for ordinary data. The final port therefore cleans
only copied executable text, in bounded 1 KiB chunks, and invalidates the
matching I-cache range before publishing the task.

Instrumentation that changes latency-sensitive NuttX scheduler paths is also
explicitly disabled. The final generated kernel configuration retains
`CONFIG_SCHED_INSTRUMENTATION_SWITCH=y` for CPU attribution, while
preemption, critical-section, and IRQ-handler tracing are all off.

The final NuttX production image completed a 600-second regular `lvmusic`
heavy-render scene plus repeated network downloads with 1,814 active render
samples. Median/mean FPS was 6/5.94; median/mean/maximum render time was
138/137.85/160 ms and flush maximum was 12 ms. During the exact measured
interval RT-scope recorded 603,070 releases and executions, zero misses, zero
late finishes, p99 and p99.9 dispatch in the `<=32 us` bucket, and a 115.15 us
maximum. Including workload startup and process teardown gives the more
conservative 643,250/643,250 executions and a 240.19 us maximum, still with
zero misses or late finishes.

For comparison, the same Iteration 7 workload on FreeRTOS recorded 1,778 active
render samples, 6/6.05 median/mean FPS, 141/141.14/293 ms
median/mean/maximum render time, zero misses, and a 77.63 us maximum dispatch.
Zephyr recorded 1,799 active samples, 6/6.06 FPS,
130/129.52/144 ms render time, zero misses, and a 63.02 us maximum dispatch.
NuttX remains slower in average dispatch because its scheduler wake path is
longer, but the multi-release cache-drain failure is removed and all three
engines remain within the product timing budget.

All 45 LXP host CTest targets pass in normal and AddressSanitizer plus
UndefinedBehaviorSanitizer builds. The coordinator suite runs 79 tests; the
feature-gate, fuzz-replay, decoupling, guest-memory repository, and Cortex-M4
QEMU gates pass. The oveRTOS C, C++, Rust, Zig, and NuttX host groups pass.
After regenerating a pre-existing incomplete Zephyr native-simulation build,
its 276 tests also pass.

FreeRTOS, NuttX, and Zephyr each passed STM32F746G-DISCO boot, guest launch,
memory-fault containment, and RT-scope hardware checks. The clean final NuttX
image was rebuilt, ST-Link programmed and verified it, and a post-flash loaded
smoke recorded 80,310/80,310 executions with zero misses or late finishes.
`uname -a` reports `NuttX 12.12.0 ove-bdd37f4 lxp-6cfad27`.

## Iteration 8 protocol and profile gates

Iteration 8 converts the stabilized lifecycle rules into a bounded protocol
test. The coordinator suite enumerates all 7,776 five-command words over park,
timeout, signal, exit, slot reuse, and stale completion. It stops a word at an
illegal model transition and invokes `lxp_validate_world()` after every legal
transition. Focused cases additionally cross signal delivery with a blocked
netfs request, exec commit with an older deferred request, group exit with a
shared address space, and slot reuse with a late completion from the dead
generation. The coordinator test target now enables the full optional
DEV/NET/NETFS/NETFS_EXEC/PTY surface instead of testing lifecycle only in a
reduced build.

Four named profiles compose existing features without selecting alternative
lifecycle code:

| Profile | Devices and input | Network / read-only 9P | Remote exec | PTY | Diagnostics | Hardening |
|---|---|---|---|---|---|---|
| Minimal | off | off / off | off | off | off | defaults |
| Full compatibility | on | on / on | on | on | board RT-scope default | defaults |
| Diagnostic | on | on / on | on | on | latency + debug + board RT-scope | defaults |
| Hardened | on | on / on | off | on | warning log, RT-scope off | stack canaries |

Every profile compiles the same app, seam, coordinator, state machine, and
world validator. The existing all-defconfig jobs therefore build a supported
three-engine by four-profile matrix on both QEMU and supported STM32 targets.

The QEMU resource gate records actual generated binaries and literal internal
`.bss` sections. Zephyr's value in parentheses is its linker-reported FLASH
span. External pool sizes include program and dynamic-link regions, externally
resident cold storage, and remote-exec staging where enabled.

| Engine | Profile | `NSLOT` / `NREG` | Flash image | Internal BSS | External pools |
|---|---|---:|---:|---:|---:|
| FreeRTOS | Minimal | 9 / 5 | 601,524 B | 451,904 B | 3,944,760 B |
| FreeRTOS | Full | 8 / 4 | 1,079,700 B | 776,540 B | 3,419,072 B |
| FreeRTOS | Diagnostic | 8 / 4 | 1,086,472 B | 781,084 B | 3,419,072 B |
| FreeRTOS | Hardened | 9 / 5 | 1,099,720 B | 782,820 B | 3,944,760 B |
| NuttX | Minimal | 10 / 6 | 225,228 B | 204,148 B | 4,718,592 B |
| NuttX | Full | 10 / 6 | 296,384 B | 232,840 B | 4,980,736 B |
| NuttX | Diagnostic | 10 / 6 | 297,780 B | 235,968 B | 4,980,736 B |
| NuttX | Hardened | 10 / 6 | 295,640 B | 232,832 B | 4,718,592 B |
| Zephyr | Minimal | 5 / 1 | 107,848 B (113,992 B) | 92,845 B | 793,432 B |
| Zephyr | Full | 5 / 1 | 218,656 B (225,824 B) | 115,055 B | 1,055,576 B |
| Zephyr | Diagnostic | 5 / 1 | 220,432 B (227,600 B) | 117,235 B | 1,055,576 B |
| Zephyr | Hardened | 5 / 1 | 217,944 B (225,112 B) | 115,043 B | 793,432 B |

Cold capture storage is 1,400 B per slot and the native slot stack is 1,024 B
per slot on all three QEMU seams. On STM32, NuttX moves both objects into
external SDRAM. The production Buildroot CPIO no longer fits beside the Zephyr
application in AN521's 4 MiB internal flash, so its runner now loads the rootfs
into the lower 15,296 KiB of PSRAM and reserves the remaining 1,088 KiB for one
guest region. Full and Diagnostic occupy 94.75% of that bounded window.

The profile and protocol implementation culminates at oveRTOS `707c0ed` and
LXP `cb48ef2`. Hardware qualification exposed one integration defect in the
newly enabled Full-profile 9P client: initialization synchronously waited up
to five seconds for connect and three seconds for each handshake reply. An
unavailable optional server could therefore starve the host coordinator past
FreeRTOS's two-second watchdog. LXP `53673b3` replaces that path with a
bounded, non-blocking connect/version/attach state machine and adds a test
which requires the initial connect timeout to be zero. oveRTOS `b130a96` pins
the fix.

All 45 normal host CTest targets pass, including 84 coordinator tests. The LXP
unit and coordinator targets pass under AddressSanitizer and UndefinedBehavior
Sanitizer, and all six existing fuzz corpora pass under the sanitizer replay
engine. All 12 QEMU engine/profile builds pass after relinking the final LXP.
The table above includes the resulting code and eight-byte connection-deadline
cost; Minimal remains byte-for-byte unchanged. The current Buildroot image is
hard-float while the AN521 validation target is intentionally soft-float and
has no FPU, so QEMU correctly rejects that image at the loader ABI gate rather
than executing an incompatible guest.

The STM32F746G-DISCO Full profile is hardware-qualified on all three engines.
Each image completed the native RTOS/Linux round trip, BusyBox init through
`rcS`, getty and inetd, and at least two RT-scope windows without a missed
release, late finish, IRQ overrun, or pending execution:

| Engine | Startup releases | Startup p99 / p99.9 / max | Next-window releases | Next-window p99 / p99.9 / max |
|---|---:|---:|---:|---:|
| FreeRTOS | 10,030 | <=10 / <=16 / 33.28 us | 10,047 | <=8 / <=8 / 8.78 us |
| NuttX | 10,120 | <=12 / <=20 / 25.33 us | 10,030 | <=10 / <=12 / 15.19 us |
| Zephyr | 10,016 | <=10 / <=16 / 53.63 us | 10,049 | <=10 / <=12 / 22.41 us |

The clean FreeRTOS and NuttX images reported
`ove-b130a96 lxp-53673b3`. Zephyr ran the byte-identical pre-commit source
state before the gitlink was recorded. The Pi gateway observed 0% packet loss
to each post-boot image; root SSH executed `uname -a` successfully on all
three. The immediate application-level socket probes on NuttX and Zephyr ran
before their network stacks became connect-ready, but the post-boot ICMP and
TCP/SSH checks confirm the steady-state network path. The board was left
running the clean FreeRTOS image after 170,767 releases and executions with
zero misses, late finishes, IRQ overruns, or pending releases.

## Closure qualification (2026-07-28)

The post-Iteration 8 closure pass culminates at oveRTOS `9c093e1` and LXP
`2cf71eb`. Iteration 9 was deliberately skipped: this pass contains no native
spawn prototype or go/no-go decision.

The wider STM32 profile sweep found six integration defects which were not
visible in the original Full-profile gate:

| Commit | Finding and resolution |
|---|---|
| LXP `2cf71eb`, oveRTOS `fee3bfa` | A completed remote-netfs exec request could leave the coordinator asleep because completion published an ordinary wakeup instead of the exec intent. Completion now publishes the pending remote-exec intent, with a host regression test, and oveRTOS pins it. |
| oveRTOS `0a33437` | The board socket smoke ran once before slower network stacks became ready. It now retries within a bounded readiness window and reports elapsed time and attempt count. |
| oveRTOS `48fc5f1` | The Linux touch provider was started even when the selected profile had guest input disabled. Provider startup is now gated by the generated guest-input feature. |
| oveRTOS `d5ca20b` | FreeRTOS Minimal could fail its second Linux run after watchdog reboot. `vTaskAllocateMPURegions()` cleared the coordinator task's regions, but the cached mapped-region index survived; a same-index access then skipped the remap and read stale SDRAM through the background alias. Resetting the cache before installing the task regions preserves the existing 11-region capacity and makes repeated boot deterministic. |
| oveRTOS `54fccd2` | NuttX Hardened's per-region protection metadata exceeded internal SRAM by 756 B at 11 regions. Minimal and Hardened now use 10 regions, leaving 2,636 B of SRAM1 headroom in the clean Hardened link. Full and Diagnostic retain 8 regions. |
| oveRTOS `9c093e1` | Zephyr's headless overlay disabled QUADSPI, although every STM32 Linux-personality profile boots its rootfs from QSPI. Full-feature profiles accidentally masked this through the framebuffer overlay. QSPI, its memory map, and flash configuration are now common; the framebuffer overlay contains only display-specific LTDC/FMC setup. |

### Ten-minute Full-profile workload

The workload starts `lvmusic`, injects the Play touch, runs the heavy render
scene, and repeatedly transfers data over the network. Each engine was observed
for 600 seconds after warm-up. The raw captures identify LXP `53673b3`;
FreeRTOS, NuttX, and Zephyr identify oveRTOS `b130a96`, `3902ebc`, and
`88601c5`, respectively. Subsequent closure commits listed above affect
publication, readiness/profile gating, and headless resource configuration,
not the measured render or periodic RT-scope hot paths.

| Metric | FreeRTOS | NuttX | Zephyr |
|---|---:|---:|---:|
| Active LVGL samples | 1,771 | 1,817 | 1,819 |
| FPS min / p05 / median / mean / p95 / max | 1 / 6 / 6 / 6.05 / 7 / 7 | 4 / 5 / 6 / 5.93 / 6 / 13 | 5 / 6 / 6 / 6.03 / 6 / 17 |
| Render ms p05 / median / mean / p95 / p99 / max | 128 / 141 / 141.82 / 150 / 167 / 665 | 127 / 139 / 138.32 / 147 / 155 / 162 | 120 / 131 / 130.36 / 138 / 140 / 148 |
| Flush ms median / p99 / max | 5 / 6 / 7 | 5 / 6 / 15 | 6 / 7 / 10 |
| LVGL CPU mean | 100.00% | 100.00% | 100.00% |
| Completed network transfers / failures | 5 / 0 | 5 / 0 | 9 / 0 |
| Network completions per minute | 0.475 | 0.475 | 0.853 |

FreeRTOS and NuttX still had an in-flight transfer when the measurement ended;
the harness stopped it during cleanup. This is not counted as a transport
failure. There were no serial reconnects or fault reports on any engine.
Zephyr has the best render distribution and highest network throughput in this
sample. NuttX is about 7 ms faster than FreeRTOS at median render and 4.7 ms
faster at the mean. FreeRTOS's 665 ms render maximum is a single long-tail
event; its p99 remains 167 ms and its p05, median, and p95 remain close to
NuttX.

| RT-scope metric | FreeRTOS | NuttX | Zephyr |
|---|---:|---:|---:|
| Accepted windows | 54 | 59 | aggregate capture |
| Releases / executions | 544,510 / 544,510 | 592,821 / 592,821 | 620,626 / 620,626 |
| Missed / late / IRQ overrun / max pending | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| Dispatch min / weighted mean / maximum | 5.96 / 9.92 / 74.69 us | 6.98 / 13.00 / 823.13 us | 5.57 / 9.20 / 57.72 us |
| Worst p99 / p99.9 bucket | <=16 / <=32 us | <=32 / <=32 us | <=32 / <=32 us |
| Window-maximum median / p95 | 34.25 / 72.41 us | 30.81 / 34.19 us | not recoverable |
| Work min / max | 5.15 / 12.11 us | 4.96 / 10.24 us | 4.94 / 9.57 us |
| SVC calls / mean / maximum | 293,196 / 17.38 / 19.96 us | not instrumented | 417,136 / 15.30 / 17.52 us |

NuttX's 823.13 us dispatch is one isolated maximum: 95% of its per-window
maxima are at or below 34.19 us, both high-percentile buckets are at or below
32 us, and no release was missed or finished late. It remains a useful
worst-observed value, not a demonstrated hard bound. Zephyr's UART stream
interleaved one RT-scope prefix with userspace output, so the JSON parser
correctly rejected it as a complete window. The totals and timing fields above
were reconstructed from the retained aggregate lines; a per-window maximum
distribution cannot be recovered from that capture.

### STM32 profile matrix

All 12 engine/profile combinations build. Every combination was also flashed
to the STM32F746G-DISCO and reached BusyBox userspace. Minimal was checked for
phase-1 launch, login, `uname`, capacity reporting, and reboot where the seam
needed a repeated-run regression. Diagnostic additionally read and executed a
hard-float FDPIC program over 9P. Hardened read over 9P but rejected remote
execution as configured. The Full profile received the extended outage,
recovery, and console-login tests.

| Engine | Minimal | Full compatibility | Diagnostic | Hardened |
|---|---|---|---|---|
| FreeRTOS | PASS; clean repeated boot | PASS; read, exec, outage/recovery, login | PASS; read and exec | PASS; read, exec denied |
| NuttX | PASS; clean final build | PASS; read, exec, outage/recovery, login | PASS; read and exec | PASS; clean build, read, exec denied |
| Zephyr | PASS; clean QSPI-rootfs build | PASS; read, exec, outage/recovery, login | PASS; read, exec and RT-scope | PASS; read, exec denied |

The Full-profile network readiness observations were 43 ms on attempt 1 for
FreeRTOS, 560 ms on attempt 3 for NuttX, and 3,796 ms on attempt 6 for Zephyr.
With the temporary 9P server removed, every engine returned bounded
`EIO` to userspace rather than hanging the coordinator; after the server
returned, both read and remote exec recovered. A deliberately incorrect
console login returned a new prompt and accepted the following root login in
3.525, 3.522, and 3.521 seconds on FreeRTOS, NuttX, and Zephyr, respectively.
Observed RT-scope windows retained zero misses, late finishes, IRQ overruns,
and pending releases throughout these checks.

`free` reports guest-region capacity rather than physical byte-addressable RAM.
One region contributes 640 KiB: 128 KiB of program storage and 512 KiB of
dynamic storage. The final Minimal/Hardened capacities are therefore 7,040 KiB
from 11 regions on FreeRTOS, 6,400 KiB from 10 regions on NuttX, and 7,680 KiB
from 12 regions on Zephyr. Full/Diagnostic use 8/8/9 regions on
FreeRTOS/NuttX/Zephyr. `NSLOT` is `NREG + 4`, so the corresponding
Minimal/Hardened slot counts are 15, 14, and 16.

### Final STM32 build footprint

These are clean linker/build reports from the final profile sweep. Zephyr's
internal-RAM values are reported in rounded KiB by its build system. NuttX's
fixed-address external pools are not part of the linker accounting.

| Engine | Profile | Internal flash | Internal RAM | Linker-accounted external SDRAM |
|---|---|---:|---:|---:|
| FreeRTOS | Minimal | 116,280 B | DTCM 46,624 B; SRAM 128,016 B | 7,737,040 B |
| FreeRTOS | Full | 245,988 B | DTCM 45,472 B; SRAM 199,152 B | 7,077,472 B |
| FreeRTOS | Diagnostic | 247,796 B | DTCM 45,472 B; SRAM 201,200 B | 7,077,472 B |
| FreeRTOS | Hardened | 242,680 B | DTCM 46,624 B; SRAM 208,616 B | 7,737,040 B |
| NuttX | Minimal | 162,060 B | SRAM1 199,772 B | fixed-address pools |
| NuttX | Full | 253,784 B | SRAM1 236,596 B | fixed-address pools |
| NuttX | Diagnostic | 257,384 B | SRAM1 237,780 B | fixed-address pools |
| NuttX | Hardened | 239,228 B | SRAM1 243,124 B | fixed-address pools |
| Zephyr | Minimal | 156,952 B | 177 KiB / 256 KiB | 8,042,560 B |
| Zephyr | Full | 287,192 B | 230 KiB / 256 KiB; DTCM 12,544 B | 7,775,200 B |
| Zephyr | Diagnostic | 289,624 B | 234 KiB / 256 KiB; DTCM 12,544 B | 7,775,200 B |
| Zephyr | Hardened | 280,988 B | 240 KiB / 256 KiB; DTCM 12,544 B | 8,303,680 B |

The apparent increase from the earlier Iteration 8 table is primarily a target
difference: that table records QEMU resource gates, while this one records the
STM32F746G-DISCO production image with display, Ethernet, QSPI, target seams,
and board-specific memory placement.

The final LXP source was rebuilt rather than tested through a stale host binary.
All 45 normal CTest targets and all 45 AddressSanitizer plus
UndefinedBehaviorSanitizer targets pass at `2cf71eb`. Hardware transcripts are
retained under `output/closure-hardware-20260728/` and
`output/closure-profile-smoke-20260728/`; workload captures and machine-readable
summaries are under `output/lvmusic-net-comparison-20260727-closure/`.

## Post-closure complexity pass

A later seam audit removed two residual implicit contracts. Spawn/resume now
carries an explicit captured-child versus parked-task action, and the
generation-qualified dispatch capability is published before an RTOS API can
schedule the task. Ports no longer infer lifecycle intent from mutable
runnable/native-handle state.

The syscall umbrella was also removed from RTOS and subsystem interfaces.
Generation identities, exec capture, immutable program input, and bootstrap
operations now have narrow headers; mutable process state lives in
`lxp_proc.h`, while `lxp_syscall.h` remains a compatibility dispatch entry
point. CPIO ingestion and startup-stack construction moved out of the syscall
dispatcher into a separately compiled bootstrap unit. Including `lxp_seam.h`
therefore no longer exposes or depends on the process representation.

The follow-up diagnostics pass also removed two implicit lifecycle contracts.
Private host-state values are now compile-time aligned with their public
diagnostic representation, and every public diagnostic enum is exhaustively
covered by its allocation-free name table. Native-task presence is qualified by
the lifecycle epoch observed by the RTOS census, so snapshots and invariant
checks cannot compare a newly transitioned slot with stale scheduler data.
Launch and teardown checkpoints take a real census instead of manufacturing an
empty native-task view.

The blocked-operation scan now returns one compact policy mask instead of
exporting a boolean for every wait subsystem. The coordinator consumes only the
distinctions it owns: polling fallback, event-driven socket readiness, and
console-reader ownership. This preserves each subsystem's retry behavior while
removing duplicated timeout classification; asynchronous console control input
also reports ordinary scan progress and is delivered without an extra
maintenance sleep.

The runner no longer accepts a second one-field-family configuration object.
Display geometry now belongs to `lxp_run_config_t` with the rootfs and console
settings, reducing the public entry contract and making all per-run values share
one lifetime. A zero dimension is republished as the documented default on every
run, so a zero-initialized later invocation cannot inherit an earlier panel
configuration.

Active network and display bindings are now core-owned run state. Host adapters
export immutable provider tables but no longer define writable LXP globals, and
the public headers no longer expose those bindings for arbitrary mutation.
`lxp_run()` publishes and clears both providers through one private operation;
isolated host tests use that same seam explicitly. The optional POSIX port now
returns its provider table through `lxp_posix_net_ops()` instead of acquiring
module state as a link-time side effect. The hardened production images remain
within 24 bytes of their pre-change flash footprints with no static-RAM change.
