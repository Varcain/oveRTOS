# Benchmarks

Per-operation latency, throughput, and memory cost of the oveRTOS API across all four language bindings (C, C++, Rust, Zig) and three RTOS backends (FreeRTOS, NuttX, Zephyr), in both heap and zero-heap allocation modes.

All numbers are measured on the **STM32F746G-DISCOVERY** (Cortex-M7 @ 216 MHz, 320 KiB SRAM, 1 MiB Flash) with `CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` (caches and flash accelerators disabled — approximates a cacheless ARM MCU). See [Methodology](methodology.md) for the full how-and-why.

## Headline: minimal overhead

Per-call wrapper-vs-native deltas sit comfortably below 10% on every binding × RTOS × mode. Per-binding *median |Δ|* values, excluding bench-design-noisy cases (`mutex_contention_2t`, the `*_throughput*` 2-thread cases, `ctx_switch`):

| Binding | Median \|Δ\| range across 6 configs | Per-call adder in absolute ns |
|---------|-------------------------------------|------------------------------|
| **C++** | 0.8–2.8% | ±0–300 ns (inlined RAII + `optional` body, mostly parity with C) |
| **Rust** | 2.0–5.4% | +100–500 ns typical, up to ~1 300 ns on value-marshalling ops |
| **Zig** | 0.7–4.0% | ±0–300 ns (comptime trampoline; pin tracker elided in release) |

The absolute-ns adders are stable across cache-on / cache-off configurations; the percentage moves because the C-binding baseline shrinks 2.5–3× with caches on. See [Per-binding analysis](per-binding.md) for outlier walk-throughs and [Wrapper-vs-native notes](wrapper-vs-native-notes.md) for the known noisy cases.

## Reports

The per-RTOS pages are **raw results only**: setup, suite/case tables, wrapper-vs-native delta. Interpretation lives in the dedicated pages below so the data can be regenerated without trampling prose.

| RTOS | Heap | Zero-heap |
|------|------|-----------|
| **FreeRTOS** | [report](freertos-heap.md) | [report](freertos-zeroheap.md) |
| **NuttX**    | [report](nuttx-heap.md)    | [report](nuttx-zeroheap.md)    |
| **Zephyr**   | [report](zephyr-heap.md)   | [report](zephyr-zeroheap.md)   |

Interpretation:

- [Heap vs zero-heap](heap-vs-zeroheap.md) — per-RTOS C-binding deltas between allocation modes.
- [Per-binding analysis](per-binding.md) — wrapper overhead per language binding.
- [Wrapper-vs-native notes](wrapper-vs-native-notes.md) — IPC caveats, FreeRTOS lifecycle costs, Rust baseline elevation, Zephyr `mutex_contention_2t` flakiness.
- [Methodology](methodology.md) — build flags, iteration counts, timing source, worst-case-timing knob, calibration procedure, reproduction commands.
