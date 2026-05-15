# Benchmarks

Per-operation latency, throughput, and memory cost of the oveRTOS API across
all four language bindings (C, C++, Rust, Zig) and three RTOS backends
(FreeRTOS, NuttX, Zephyr), in both heap and zero-heap allocation modes.

All numbers below are measured on the **STM32F746G-DISCOVERY** development
board (Cortex-M7 @ 216 MHz, 320 KiB SRAM, 1 MiB Flash) running the bench app
at `tests/benchmarks/`. The same C harness drives every binding so the
runs are directly comparable.

## What we measure

For every primitive (mutex, semaphore, event, condvar, queue, stream,
timer, eventgroup, workqueue) we run three classes of test:

| Class | Meaning |
|-------|---------|
| **Latency** | Wall-clock cost of a single operation (e.g. `lock+unlock`), with statistical filtering |
| **Throughput** | Two-thread contention runs (e.g. mutex contention, queue producer/consumer) |
| **Memory** | Heap delta of `create()` (heap mode only — gated out under zero-heap) |

Each binding runs the exact same suite. The C suite calls oveRTOS C
directly; the C++/Rust/Zig suites call the binding wrappers, which in turn
inline to the same FFI symbols. The **wrapper-vs-native delta** column in
each report quantifies binding overhead within a single hardware run, so
it isolates wrapper cost from cross-run scheduler noise.

There is also a per-RTOS `native_<rtos>` suite that calls the raw
backend API (FreeRTOS `xSemaphoreTake`, NuttX `nxsem_*`, Zephyr `k_sem_*`)
identically across every binding. Those rows show **scheduler noise** more
than binding cost — the meaningful comparison is the wrapper-vs-native
table below each main report.

## Allocation modes

Two configurations are benchmarked separately:

- **Heap mode** — `_create()` / `_destroy()` API; objects are kernel-heap-allocated
- **Zero-heap mode** (`CONFIG_OVE_ZERO_HEAP=y`) — `_init()` / `_deinit()` API
  with caller-supplied static storage; the heap is locked at `ove_run()` and
  any post-boot allocation aborts the build (compile-time check) or panics
  (runtime trap)

Zero-heap is the production-recommended mode for safety-critical or
memory-constrained deployments. Heap mode is more ergonomic for prototyping.

## Methodology

- **Build flags**: `-O2 -fno-omit-frame-pointer` for all binaries (frame
  pointers are kept for the trace-view backtrace walker; benchmarks
  themselves don't unwind during measurement)
- **Iterations**: 1000 per latency case, 100 per long-running case
  (e.g. `delay_1ms`), 500 per context-switch / event-signal case.
  These were settled by the calibrate-then-lock procedure described
  below.
- **Warmup**: 100 iterations dropped before measurement starts
- **Timing**: on ARMv7-M targets the bench harness reads the DWT cycle
  counter (`DWT->CYCCNT` at `0xE0001004`) directly via a single volatile
  load — uniform across FreeRTOS / NuttX / Zephyr, so no per-RTOS
  counter-read overhead leaks into the per-call deltas. Per-measurement
  floor ≈ 50 ns (two LDRs at 216 MHz). On non-ARM targets (POSIX, sim)
  the harness falls back to `ove_time_get_ns`. Source:
  `tests/benchmarks/c/include/bench_cyccnt.h`.
- **Statistics**: min, p50, p95, p99, max, trimmed mean (top 1% dropped),
  per-op nanoseconds, Welford running stddev; ops/sec computed from the
  trimmed mean
- **Hot-path audit**: every reported binding has its hot-path disassembly
  audited against the allowed-callee list at `tests/audit/hotpath_expected.yaml` —
  any unexpected callee (alloc, exception unwind, trampoline that wasn't
  inlined) fails the build before a number reaches this page

The audit guarantees that the wrapper hot-path actually contains *only* the
expected `ove_*` FFI calls (or directly-inlined backend calls), so the
measured deltas reflect real wrapper cost and not silent indirection.

### Worst-case timing mode

The published numbers are taken with `CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y`
on STM32F7.  When set, the harness disables every hardware feature that
hides flash-fetch latency or otherwise injects per-call non-determinism:

- Cortex-M7 **I-cache** and **D-cache** (`SCB_DisableICache` /
  `SCB_DisableDCache`)
- Cortex-M7 **branch prediction** (clears `SCB->CCR` bit 18)
- STM32F7 **ART accelerator** + **flash prefetch buffer** (clears
  `FLASH->ACR` `ARTEN` and `PRFTEN`)

The motivation: many ARM microcontrollers in oveRTOS's target class
(Cortex-M0+, M3, lower-end M4) ship without caches or accelerators.
A bench taken with all of those on would overstate real-world
performance for that class of target.  With this knob on the F746
benches behave like a cacheless ARM MCU at 216 MHz, which is a
defensible upper bound on per-op cost across the supported part
families.  The toggle is applied once at the first bench-case entry
in `bench_apply_diagnostics_once()` (see `bench_harness.c`) and
gated on `CONFIG_OVE_BOARD_STM32F746G_DISCO`.

### Iteration-count calibration

Iteration counts were not picked by intuition — they were settled by
running the bench in noise-audit mode
(`CONFIG_OVE_BENCHMARK_NOISE_AUDIT=y`) on the same hardware, with
worst-case timing also enabled, and observing how CV (stddev / mean)
converges with iteration count.  The harness snapshots running mean
and stddev at iteration counts 100, 500, 1000, 2500, 5000, 10000 and
emits them as an `audit` array per case in the JSON envelope.
`scripts/bench_audit.py` reads the envelopes, computes CV at each
checkpoint per case, and recommends a per-class iteration count
defined as the smallest N where either CV ≤ 2% or doubling N reduces
CV by less than 5% of its current value (the elbow).  The class
recommendation is the maximum across all cases in that class so
every case in the class converges.

The audit Kconfig is `default n` — flip it on for calibration runs,
read the produced report, lock the recommended counts in via
per-case `iterations` overrides or `CONFIG_OVE_BENCHMARK_ITERATIONS`,
then turn it off again for production runs.

## Reports

The per-RTOS pages below are **raw results only**: setup description,
suite/case tables, and the wrapper-vs-native within-run delta table.
Interpretation lives in dedicated pages so the data can be regenerated
from a fresh bench run without trampling prose.

| RTOS | Heap | Zero-heap |
|------|------|-----------|
| **FreeRTOS** | [report](freertos-heap.md) | [report](freertos-zeroheap.md) |
| **NuttX**    | [report](nuttx-heap.md)    | [report](nuttx-zeroheap.md)    |
| **Zephyr**   | [report](zephyr-heap.md)   | [report](zephyr-zeroheap.md)   |

Interpretation pages:

- [Heap vs zero-heap](heap-vs-zeroheap.md) — per-RTOS C-binding deltas
  between allocation modes
- [Per-binding analysis](per-binding.md) — wrapper overhead per
  language binding (C, C++, Rust, Zig)
- [Wrapper-vs-native notes](wrapper-vs-native-notes.md) — IPC caveats,
  FreeRTOS lifecycle/intrinsic costs, Rust same-process baseline
  elevation, Zephyr `mutex_contention_2t` flakiness

## Reproducing

To run the heap-mode benches yourself on STM32F746G-DISCO hardware:

```bash
# FreeRTOS / NuttX / Zephyr
make benchmarks-stm32f746g-discovery
make benchmarks-stm32f746g-discovery-nuttx
make benchmarks-stm32f746g-discovery-zephyr
```

Each invocation builds all four bindings, flashes the board, captures
the serial output, generates a Markdown report at
`output/stm32f746/<rtos>/_benchmarks/report.md`, and emits raw
per-binding logs alongside.

The zero-heap reports use the same `make benchmarks-...` target with
the `ZEROHEAP=1` flag (or `ove benchmarks --zeroheap <platform>`
directly).  The runner builds the `_zh` apps, flashes them, captures
the serial output, and writes the report straight to
`docs-site/docs/benchmarks/<rtos>-zeroheap.md`:

```bash
make benchmarks-stm32f746g-discovery        ZEROHEAP=1   # FreeRTOS zh
make benchmarks-stm32f746g-discovery-nuttx  ZEROHEAP=1   # NuttX zh
make benchmarks-stm32f746g-discovery-zephyr ZEROHEAP=1   # Zephyr zh
```

## What "minimal overhead" means

Across the published reports the per-call wrapper-vs-native deltas
sit comfortably below 10% on every binding × RTOS × mode.  Per-binding
*median |Δ|* values, excluding bench-design-noisy cases
(`mutex_contention_2t`, the `*_throughput*` 2-thread cases,
`ctx_switch`):

| Binding | Median \|Δ\| range across 6 configs | Per-call adder in absolute ns |
|---------|-------------------------------------|------------------------------|
| **C++** | 0.8–2.8% | ±0–300 ns (inlined RAII + `optional` body, mostly parity with C) |
| **Rust** | 2.0–5.4% | +100–500 ns typical, up to ~1 300 ns on value-marshalling ops (fixed: `Result<T,E>` wrap, `Error::from_code`, `Option` decode) |
| **Zig** | 0.7–4.0% | ±0–300 ns (comptime trampoline; pin tracker elided in release) |

These percentages are the worst-case-timing numbers (caches and
accelerators off — see the section above).  The absolute-ns adder
column is the *same* under cache-on configurations, but the C-binding
base op is then 2.5–3× smaller, so the same fixed adapter shows as a
larger percentage.  The wrappers haven't gotten cheaper between
configurations; the comparison floor moved.

Many operations measure at near-zero overhead (`mutex_lock_unlock`,
`sem_take_give`, `thread/yield`, `recursive_mutex_lock_unlock` —
wrappers are direct FFI passthroughs and inline cleanly under `-O2`).
Others carry a measurable binding-side cost.  Rust's `Result<T, E>`
wrapping is the most consistent adder, visible on the tightest ops
(`time_get_us_overhead`, `queue/send_receive`).  C++ and Zig stay at
parity with C on per-call sync; the few outliers above 10% are on
stream-buffer paths where compiled-code-size differences between
bindings show up directly on a flash-fetch-bound execution path.
The [per-binding analysis](per-binding.md) walks every outlier with
the underlying mechanism.

Where a per-call hot-path delta exceeds ~10%, the report flags it
explicitly in a "Cases with |Δ| > 10.0% vs C" section so the cost is
visible rather than buried in averages.  Outside two known categories —
`mutex_contention_2t` flakiness on Zephyr (kernel scheduler quirk,
see [wrapper-vs-native notes](wrapper-vs-native-notes.md)) and a small
number of stream-buffer outliers attributable to compiled wrapper-code
size — those lists are short.
