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

## Results

Per-op cost of the **personality** side (min, cycles @216 MHz) and the **tax**
(personality ÷ native for the same axis):

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

## What it says

- **A syscall is cheap and engine-independent** — the SVC boundary is
  **~1100–1700 cycles** (5–8 µs) everywhere. Pure compute is nearly free (≈1×)
  when the program runs privileged and cached.
- **Multi-process cost diverges ~200× across engines** — this dominates. On
  **FreeRTOS** a context switch is **197 µs** and a spawn **7.4 ms**, because its
  coordinator is *event-driven* (the dispatch posts a semaphore the coordinator
  waits on). **NuttX** (7.8 ms / 24 ms) and **Zephyr** (40 ms / 95 ms) pay
  ms-scale latencies — their cross-process wake is tick/work-queue-quantized, and
  Zephyr adds K_USER MPU domain switches on top.
- **Zephyr's isolation has a compute cost** — B1 is **3.5×** (vs 1.1× elsewhere)
  because the unprivileged program's K_USER text region executes **uncached**,
  unlike the privileged FreeRTOS/NuttX path from cacheable flash. A real cost of
  stronger isolation (and a candidate optimisation — an MPU cache-attribute fix).

!!! note "Takeaway"
    Running Linux software costs ~1–2k cycles per syscall regardless of engine,
    but the **choice of engine dominates multi-process performance**:
    FreeRTOS's event-driven coordinator is 40–200× ahead of NuttX/Zephyr for IPC
    and process spawn.

## Reproduce

See
[`tests/benchmarks/linux/README.md`](https://github.com/varcain/oveRTOS/tree/main/tests/benchmarks/linux):
build the native bench (`WORST_CASE_TIMING=n`, caches on) and the personality
firmware per engine, flash, capture `/dev/ttyACM0`, and run `lbench_compare.py`.
QEMU validates the harness but is **not** cycle-accurate — take numbers on
hardware.
