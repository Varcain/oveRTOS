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
