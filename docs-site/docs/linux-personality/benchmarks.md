# Benchmarks: the personality tax

How much does running work as an **unprivileged FDPIC Linux process** (SVC
syscall boundary + MPU + run-loop coordinator) cost versus the *same* work as a
**native `ove_thread`**? Measured on real **STM32F746** silicon (Cortex-M7
@ 216 MHz, caches on) across all three engines.

## Method

Two firmware images, one clock. The native side is the
[`tests/benchmarks/c`](https://github.com/varcain/oveRTOS/tree/main/tests/benchmarks/c)
app; the guest side is `/usr/bin/lbench` (Buildroot `board/overtos/progs/lbench.c`),
a byte-identical mirror. Both time off the **DWT cycle counter** — the native
harness reads `DWT->CYCCNT` directly, the guest reads `clock_gettime`, which the
personality backs with the same register — so the figures are directly
comparable. `lbench_compare.py` pairs them per axis. The B1 compute kernel is
identical in both builds (cross-checked by a checksum), so its ≈1× ratio on the
privileged engines confirms the comparison is fair.

## Results (baseline)

Per-op cost of the **personality** side (min, cycles @216 MHz) and the **tax**
(personality ÷ native for the same axis), as first measured (commit `8459073`):

| Axis | FreeRTOS | NuttX | Zephyr |
|------|----------|-------|--------|
| **compute** (256-round kernel) | 1.9 Kc · **1.1×** | 1.9 Kc · **1.1×** | 6.4 Kc · **3.5×** |
| **null syscall** (SVC round-trip) | 1.1 Kc · 82× | 1.7 Kc · 123× | 1.4 Kc · 33× |
| **write()** to /dev/null | 1.3 Kc | 1.8 Kc | 1.9 Kc |
| **ctx-switch** (pipe round-trip) | 42.6 Kc · 12× | **1.69 Mc** · 385× | **8.75 Mc** · 1815× |
| **pipe 4 KiB** (drain-limited) | 148 Kc (~6 MB/s) | 3.45 Mc | 17.7 Mc |
| **spawn** (vfork+execve+wait) | 1.59 Mc · 218× | 5.2 Mc · 92× | 20.6 Mc · 1546× |

*(Native floors differ per engine — e.g. NuttX thread-create is slow (kmm alloc
per task), which deflates its spawn **ratio** even though its absolute spawn is
3× FreeRTOS's. Compare absolutes across a row, not just ratios.)*

## Results (after the optimisation pass)

The baseline exposed three cost classes — Zephyr uncached compute, ms-scale
cross-process wake on NuttX/Zephyr, and coordinator-bound pipe throughput. Each
was attacked at the root; personality min cost, **before → after** on the same
silicon:

| Axis | FreeRTOS | NuttX | Zephyr |
|------|----------|-------|--------|
| **compute** | 1.9 Kc *(unch.)* | 1.9 Kc *(unch.)* | **6.4 → 1.7 Kc** · 3.8× |
| **null syscall** | 1.13 → 1.06 Kc | 1.71 → 1.46 Kc · −15% | 1.43 → 1.15 Kc · −20% |
| **clock_gettime** | 1.8 Kc *(+ wrap fix)* | 2.26 → 2.01 Kc | 3.11 → 1.99 Kc · −36% |
| **ctx-switch** | 197 µs *(unch.)* | 7.8 → 5.6 ms · −28% | **40.5 ms → 300 µs** · 135× |
| **pipe 4 KiB** | 685 → 224 µs · 3× | 16 ms → 243 µs · 66×† | **82 ms → 600 µs** · 137× |
| **spawn** | 7.4 ms *(unch.)* | 24 → 17.8 ms · −26% | **95 → 7.4 ms** · 13× |

† NuttX pipe: *best-case* (a single 4 KiB write into the enlarged ring) is
243 µs; *sustained* p50 stays ~5.9 ms — bound by per-hop task churn (below).

### Optimisations applied

- **Zephyr I-cache** (`CACHE_MANAGEMENT=y`, D-cache off) — the K_USER text now
  executes cached; compute 3.5× → 1.1×, every Zephyr row drops.
- **Zephyr wake-path PendSV** — the program `svc` oops-path return skipped the
  scheduler, so a readied coordinator waited out the parked program's 20 ms
  timeslice. Pending PendSV from `event_post` (as `z_arm_exc_exit` would) makes
  the wake immediate: ctx-switch **135×**, pipe **137×**, spawn **13×**. Zephyr
  now matches FreeRTOS.
- **4 KiB pipe ring** + two-segment `memcpy` — a typical write is copy-bound, not
  a park/resume round trip per 1 KiB (FreeRTOS 3×, Zephyr 137×). 2 KiB on Zephyr
  (its K_USER domains leave less SRAM).
- **NuttX kernel −O2** + note-driver guard (skip the 6-write MPU reprogram when
  the region is unchanged) — syscall/ctx/spawn −15…−28%.
- **FreeRTOS 64-bit cycle epoch** — a real correctness fix: guest
  `CLOCK_MONOTONIC` no longer wraps every **19.86 s** (verified across the wrap
  on hardware), and the per-read `__aeabi_uldivmod` becomes a Q32 multiply.
- **FreeRTOS `current_slot`** direct `pxCurrentTCB` read (drop the MPU-wrapped
  `xTaskGetCurrentTaskHandle`); **D-cache clean skipped** at exec when D-cache is
  off.

## What it says

- **A syscall is cheap and engine-independent** — the SVC boundary is now
  **~1.1–1.5 Kc** (5–7 µs) everywhere. Pure compute is nearly free (≈1.1×) on
  *every* engine — Zephyr's isolation no longer costs a 3.5× compute penalty.
- **Cross-process cost, once ~200× divergent, is now within ~2× on FreeRTOS and
  Zephyr.** The dominant baseline gap was Zephyr's wake path skipping the
  scheduler; with PendSV pended, its context switch (300 µs) and spawn (7.4 ms)
  match FreeRTOS. IPC and spawn are **sub-ms / single-digit-ms** on both.
- **NuttX remains ms-scale for the multi-process axes** (ctx-switch 5.6 ms,
  spawn 17.8 ms) — instrumentation showed its wake *is* already event-driven; the
  cost is per-hop **task churn** (`nxtask_init` + activate + `task_delete` every
  resume, ≈18× Zephyr's `k_thread_create`). The single-write pipe case is fast
  (243 µs) because it avoids the hop. Collapsing the churn to persistent
  block/unblock slot-tasks is the one remaining lever — a deferred concurrency
  refactor, not a config flip.

!!! note "Takeaway"
    After the pass, a syscall is ~1 Kc on every engine and multi-process IPC/spawn
    is **sub-millisecond to single-digit-millisecond on FreeRTOS and Zephyr**.
    Zephyr went from 40 ms to 300 µs per context switch (135×) once its wake path
    stopped skipping the scheduler. NuttX's per-hop RTOS-task rebuild is the last
    ms-scale cost, and the only remaining structural optimisation.

## Reproduce

See
[`tests/benchmarks/linux/README.md`](https://github.com/varcain/oveRTOS/tree/main/tests/benchmarks/linux):
build the native bench (`WORST_CASE_TIMING=n`, caches on) and the personality
firmware per engine, flash, capture `/dev/ttyACM0`, and run `lbench_compare.py`.
QEMU validates the harness but is **not** cycle-accurate — take numbers on
hardware.
