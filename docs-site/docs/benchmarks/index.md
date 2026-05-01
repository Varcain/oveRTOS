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
  (e.g. `delay_1ms`), 500 per context-switch / event-signal case
- **Warmup**: 100 iterations dropped before measurement starts
- **Timing**: on ARMv7-M targets the bench harness reads the DWT cycle
  counter (`DWT->CYCCNT` at `0xE0001004`) directly via a single volatile
  load — uniform across FreeRTOS / NuttX / Zephyr, so no per-RTOS
  counter-read overhead leaks into the per-call deltas. Per-measurement
  floor ≈ 50 ns (two LDRs at 216 MHz). On non-ARM targets (POSIX, sim)
  the harness falls back to `ove_time_get_ns`. Source:
  `tests/benchmarks/c/include/bench_cyccnt.h`.
- **Statistics**: min, p50, p95, p99, max, trimmed mean (top 1% dropped),
  per-op nanoseconds; ops/sec computed from the trimmed mean
- **Hot-path audit**: every reported binding has its hot-path disassembly
  audited against the allowed-callee list at `tests/audit/hotpath_expected.yaml` —
  any unexpected callee (alloc, exception unwind, trampoline that wasn't
  inlined) fails the build before a number reaches this page

The audit guarantees that the wrapper hot-path actually contains *only* the
expected `ove_*` FFI calls (or directly-inlined backend calls), so the
measured deltas reflect real wrapper cost and not silent indirection.

## Reports

| RTOS | Heap | Zero-heap |
|------|------|-----------|
| **FreeRTOS** | [report](freertos-heap.md) | [report](freertos-zeroheap.md) |
| **NuttX**    | [report](nuttx-heap.md)    | [report](nuttx-zeroheap.md)    |
| **Zephyr**   | [report](zephyr-heap.md)   | [report](zephyr-zeroheap.md)   |

## Reproducing

To run the benches yourself on STM32F746G-DISCO hardware:

```bash
# FreeRTOS
make benchmarks-stm32f746g-discovery               # heap mode
make benchmarks-stm32f746g-discovery ZEROHEAP=1    # zero-heap mode

# NuttX
make benchmarks-stm32f746g-discovery-nuttx
make benchmarks-stm32f746g-discovery-nuttx ZEROHEAP=1

# Zephyr
make benchmarks-stm32f746g-discovery-zephyr
make benchmarks-stm32f746g-discovery-zephyr ZEROHEAP=1
```

Each invocation builds all four bindings, flashes the board, captures the
serial output, generates a Markdown report at
`output/stm32f746/<rtos>/_benchmarks[_zeroheap]/report.md`, and emits raw
per-binding logs alongside.

## What "minimal overhead" means

Looking across the reports below, you'll see the wrapper-vs-native deltas
for the per-op hot paths typically sit in the single-digit-percent range.
Some operations approach zero (mutex lock/unlock, semaphore take/give —
wrappers are direct FFI passthroughs and inline cleanly under `-O2`).
Others carry a binding-specific cost: Rust's `Result<T, E>` wrapping
costs ~80–150 ns per call (visible cleanly now that the harness's own
counter-read overhead is uniform across RTOSes), C++'s `std::optional`
for storage adds an engaged-bit check that occasionally collides with
the wrapped buffer's cache line, Zig's debug-build pinning check is
compiled out in release.

Where you see a delta exceeding ~10%, the report flags it explicitly in a
"Cases with |Δ| > 10.0% vs C" section so the cost is visible rather than
buried in averages. We work to keep that list short.
