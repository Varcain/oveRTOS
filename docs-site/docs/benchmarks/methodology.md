# Benchmark methodology

!!! note "Audience"
    This page is for **oveRTOS developers** and anyone reproducing or interrogating the benchmark numbers. App developers can skip to [Benchmarks overview](index.md).

## What we measure

For every primitive (mutex, semaphore, event, condvar, queue, stream, timer, eventgroup, workqueue) we run three classes of test:

| Class | Meaning |
|-------|---------|
| **Latency** | Wall-clock cost of a single operation (e.g. `lock+unlock`), with statistical filtering |
| **Throughput** | Two-thread contention runs (e.g. mutex contention, queue producer/consumer) |
| **Memory** | Heap delta of `create()` (heap mode only — gated out under zero-heap) |

Each binding runs the exact same suite. The C suite calls oveRTOS C directly; the C++/Rust/Zig suites call the binding wrappers, which inline to the same FFI symbols. The **wrapper-vs-native delta** column in each report quantifies binding overhead within a single hardware run, so it isolates wrapper cost from cross-run scheduler noise.

A per-RTOS `native_<rtos>` suite calls the raw backend API (FreeRTOS `xSemaphoreTake`, NuttX `nxsem_*`, Zephyr `k_sem_*`) identically across every binding. Those rows show **scheduler noise** more than binding cost — the meaningful comparison is the wrapper-vs-native table below each main report.

## Allocation modes

Two configurations are benchmarked separately:

- **Heap mode** — `_create()` / `_destroy()` API; objects are kernel-heap-allocated.
- **Zero-heap mode** (`CONFIG_OVE_ZERO_HEAP=y`) — `_init()` / `_deinit()` API with caller-supplied static storage; the heap is locked at `ove_run()` and any post-boot allocation aborts the build (compile-time check) or panics (runtime trap).

## Build & timing

- **Build flags**: `-O2 -fno-omit-frame-pointer` (frame pointers kept for the trace-view backtrace walker; benchmarks don't unwind during measurement).
- **Iterations**: 1000 per latency case, 100 per long-running case (e.g. `delay_1ms`), 500 per context-switch / event-signal case. Settled by the calibrate-then-lock procedure below.
- **Warmup**: 100 iterations dropped before measurement starts.
- **Timing**: on ARMv7-M targets the harness reads the DWT cycle counter (`DWT->CYCCNT` at `0xE0001004`) directly via a single volatile load — uniform across FreeRTOS / NuttX / Zephyr, so no per-RTOS counter-read overhead leaks into the per-call deltas. Per-measurement floor ≈ 50 ns (two LDRs at 216 MHz). On non-ARM targets (POSIX, sim) the harness falls back to `ove_time_get_ns`. Source: `tests/benchmarks/c/include/bench_cyccnt.h`.
- **Statistics**: min, p50, p95, p99, max, trimmed mean (top 1% dropped), per-op nanoseconds, Welford running stddev; ops/sec computed from the trimmed mean.
- **Hot-path audit**: every reported binding has its hot-path disassembly audited against the allowed-callee list at `tests/audit/hotpath_expected.yaml` — any unexpected callee (alloc, exception unwind, trampoline that wasn't inlined) fails the build before a number reaches a report.

## Worst-case timing mode

The published numbers are taken with `CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` on STM32F7. When set, the harness disables every hardware feature that hides flash-fetch latency or otherwise injects per-call non-determinism:

- Cortex-M7 **I-cache** and **D-cache** (`SCB_DisableICache` / `SCB_DisableDCache`)
- Cortex-M7 **branch prediction** (clears `SCB->CCR` bit 18)
- STM32F7 **ART accelerator** + **flash prefetch buffer** (clears `FLASH->ACR` `ARTEN` and `PRFTEN`)

Motivation: many ARM microcontrollers in oveRTOS's target class (Cortex-M0+, M3, lower-end M4) ship without caches or accelerators. A bench taken with all of those on would overstate real-world performance for that class of target. With this knob on, the F746 benches behave like a cacheless ARM MCU at 216 MHz — a defensible upper bound on per-op cost across the supported part families. The toggle is applied once at the first bench-case entry in `bench_apply_diagnostics_once()` (see `bench_harness.c`) and gated on `CONFIG_OVE_BOARD_STM32F746G_DISCO`.

## Iteration-count calibration

Iteration counts were settled by running the bench in noise-audit mode (`CONFIG_OVE_BENCHMARK_NOISE_AUDIT=y`) on the same hardware, with worst-case timing also enabled, and observing how CV (stddev / mean) converges with iteration count. The harness snapshots running mean and stddev at iteration counts 100, 500, 1000, 2500, 5000, 10000 and emits them as an `audit` array per case in the JSON envelope. `scripts/bench_audit.py` reads the envelopes, computes CV at each checkpoint per case, and recommends a per-class iteration count defined as the smallest `N` where either CV ≤ 2% or doubling `N` reduces CV by less than 5% of its current value (the elbow). The class recommendation is the maximum across all cases in that class so every case in the class converges.

The audit Kconfig is `default n` — flip it on for calibration runs, read the produced report, lock the recommended counts in via per-case `iterations` overrides or `CONFIG_OVE_BENCHMARK_ITERATIONS`, then turn it off again for production runs.

## Reproducing

Run the heap-mode benches on STM32F746G-DISCO hardware:

```bash
make benchmarks-stm32f746g-discovery        # FreeRTOS
make benchmarks-stm32f746g-discovery-nuttx
make benchmarks-stm32f746g-discovery-zephyr
```

Each invocation builds all four bindings, flashes the board, captures the serial output, generates a Markdown report at `output/stm32f746/<rtos>/_benchmarks/report.md`, and emits raw per-binding logs alongside.

The zero-heap reports use the same target with `ZEROHEAP=1` (or `ove benchmarks --zeroheap <platform>` directly), writing straight to `docs-site/docs/benchmarks/<rtos>-zeroheap.md`:

```bash
make benchmarks-stm32f746g-discovery        ZEROHEAP=1   # FreeRTOS zh
make benchmarks-stm32f746g-discovery-nuttx  ZEROHEAP=1   # NuttX zh
make benchmarks-stm32f746g-discovery-zephyr ZEROHEAP=1   # Zephyr zh
```
