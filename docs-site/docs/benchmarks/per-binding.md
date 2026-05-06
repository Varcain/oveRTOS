# Per-language binding analysis

Companion to [Heap vs zero-heap](heap-vs-zeroheap.md): for each
language binding (C, C++, Rust, Zig), what does the wrapper layer
actually cost vs calling the same `ove_*` FFI from C?

All numbers below are taken from the just-published per-RTOS reports
under `CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` — Cortex-M7 I-cache,
D-cache, branch predictor, ART accelerator, and flash prefetch all
disabled (see [benchmarks overview](index.md)).  Two consequences for
how to read this page:

1. **Every per-iteration flash fetch costs the full flash latency.**
   There are no hidden cache hits.  Wrapper code that compiles to more
   bytes of flash takes proportionally longer than wrapper code that
   compiles to fewer.
2. **No cache-line locality argument applies** to anything below.
   When a non-C binding is faster or slower than C by a few hundred
   nanoseconds on the same op, that's compiler codegen plus
   wrapper-code-size, not L1 D-cache placement.

## Coverage matrix

| Binding | FreeRTOS heap | FreeRTOS ZH | NuttX heap | NuttX ZH | Zephyr heap | Zephyr ZH |
|---------|:-------------:|:-----------:|:----------:|:--------:|:-----------:|:---------:|
| **C**   | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **C++** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **Rust**| ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **Zig** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

All bench bindings call `ove_thread_start_scheduler()` directly
instead of `ove_run()` — the bench's create/destroy cases require
post-init kernel allocation, which `ove_run()`'s zero-heap auto-lock
would block on NuttX.  C/CPP have always done this; Rust exposes
`ove::start_scheduler()` and Zig `ove.startScheduler()` for the same
purpose.

## Headline: per-call median \|Δ\| vs C

Excluding three categories of bench-design noise — `mutex_contention_2t`
(Zephyr timeslicing, see [wrapper-vs-native notes](wrapper-vs-native-notes.md)),
the `*_throughput*` cases (2-thread producer/consumer where scheduling
alignment dominates), and `ctx_switch` (the ping-pong shape doubles
the wrapper cost) — the remaining per-call hot paths give:

| Config | C++ median / max | Rust median / max | Zig median / max |
|---|---:|---:|---:|
| FreeRTOS heap | 2.8% / 32% | 2.2% / 74% | 0.7% / 62% |
| FreeRTOS ZH   | 0.8% / 8%  | 3.6% / 26% | 4.0% / 39% |
| NuttX heap    | 0.9% / 6%  | 2.0% / 12% | 1.5% / 6%  |
| NuttX ZH      | 1.4% / 11% | 3.6% / 21% | 1.1% / 14% |
| Zephyr heap   | 2.5% / 9%  | 5.4% / 27% | 2.2% / 31% |
| Zephyr ZH     | 1.5% / 74% | 2.8% / 25% | 1.2% / 12% |

**Median \|Δ\| sits at 0.7–5.4% for every binding × RTOS × mode.**  No
binding-level overhead exceeds 6% on the typical path.  The "max"
column is dominated by stream-buffer outliers (more on these below);
the median is what most workloads will see.

## Per-call wrapper cost in absolute nanoseconds

Same comparison, but in absolute ns rather than %.  For five per-call
hot paths across three RTOSes:

### FreeRTOS heap (cache-off baseline)

| Case | C | Δ C++ | Δ Rust | Δ Zig |
|---|---:|---:|---:|---:|
| `time/time_get_us_overhead` | 883 ns | +317 ns | +217 ns | +117 ns |
| `thread/yield` | 4 500 ns | −200 ns | −100 ns | 0 ns |
| `sync/mutex_lock_unlock` | 8 100 ns | +100 ns | +400 ns | −100 ns |
| `sync/sem_take_give` | 6 500 ns | −100 ns | +100 ns | −100 ns |
| `queue/send_receive` | 8 200 ns | 0 ns | +1 100 ns | +300 ns |

### NuttX heap

| Case | C | Δ C++ | Δ Rust | Δ Zig |
|---|---:|---:|---:|---:|
| `time/time_get_us_overhead` | 3 000 ns | +100 ns | +100 ns | 0 ns |
| `thread/yield` | 3 600 ns | +200 ns | +100 ns | −100 ns |
| `sync/mutex_lock_unlock` | 5 400 ns | 0 ns | −100 ns | +100 ns |
| `sync/sem_take_give` | 4 300 ns | −100 ns | +500 ns | −100 ns |
| `queue/send_receive` | 11 800 ns | −200 ns | +100 ns | +700 ns |

### Zephyr heap

| Case | C | Δ C++ | Δ Rust | Δ Zig |
|---|---:|---:|---:|---:|
| `time/time_get_us_overhead` | 2 500 ns | −100 ns | +100 ns | 0 ns |
| `thread/yield` | 10 600 ns | 0 ns | 0 ns | −200 ns |
| `sync/mutex_lock_unlock` | 3 000 ns | +100 ns | +400 ns | −200 ns |
| `sync/sem_take_give` | 2 000 ns | +200 ns | +100 ns | +100 ns |
| `queue/send_receive` | 4 700 ns | −400 ns | +1 300 ns | −300 ns |

**Three patterns from these tables:**

1. **C++ and Zig are at parity with C** on per-call sync — within
   ±300 ns, which is at or below the worst-case-timing scheduler
   noise floor.  Negative deltas (C++/Zig "faster than C") show up
   wherever g++/zig emits one fewer flash fetch than gcc on the
   same FFI call shape — they're real but small.
2. **Rust adds a fixed adapter cost** that's typically +100–500 ns
   per call on simple ops (`mutex_lock`, `sem_take_give`,
   `time_get_us`, `thread/yield`) and grows to +1 000–1 300 ns on
   ops where the wrapper does more work
   (`queue/send_receive` reads + writes a value through `Result<T>`,
   so the adapter cost compounds with the value transfer).  This
   matches the `Result<T, ove::Error>` wrap + `Error::from_code(rc)`
   shape in `ove::*` Rust API.
3. **NuttX has the smallest cross-binding spread.** Median \|Δ\|
   across all 4 bindings × all NuttX hot paths is well under 200 ns.
   NuttX baseline ops are larger (4–12 µs vs Zephyr's 2–4 µs), so
   the same fixed adders are a smaller fraction of total time AND
   produce less variance because more code is shared between
   wrapper and native paths.

## C++

The C++ wrappers are header-only RAII classes (`ove::Mutex`,
`ove::Queue`, …) using `std::optional` for setup/teardown lifecycle.
Methods inline cleanly under `-O2`.

**Headline: median \|Δ\| 0.8–2.8% per call. The tightest non-C binding
on most configs.**

Outliers worth knowing:

- **FreeRTOS heap `thread/create_destroy` −68% / `workqueue/create_destroy`
  −67%**: the C++ `Thread (ctor+dtor)` and `WorkQueue (ctor+dtor)` use
  static stack memory rather than the kernel-pool allocation the C
  wrapper does.  Different ownership semantics, not a "C++ faster
  than C" win in any philosophical sense.  Both gated out under
  zero-heap (where `*_create_destroy` cases don't compile).
- **Zephyr ZH `stream/send_recv_64B` +74%** (14.3 → 24.8 µs): a
  +10 µs absolute regression vs C on a 14 µs base.  The C++
  `ove::Stream::send_recv` wrapper compiles to more bytes than the
  C `ove_stream_send` + `ove_stream_receive` pair on Zephyr ZH,
  and on a flash-fetch-bound execution path that extra code size
  shows up directly.  Persistent across runs but Zephyr-zh-specific:
  `Zephyr heap` shows parity (14.3 vs 14.3) and `FreeRTOS zh` shows
  parity (32.9 vs 32.9).  Worth a closer look at the Zephyr ZH
  release-build assembly for `Stream::send_recv` if that path is on
  your hot path.

For **per-call sync primitives** (lock/unlock, take/give, signal/wait,
yield) C++ stays inside ±300 ns of C on every RTOS+mode combination.
The ergonomic wins (RAII, type-safe queue) come at zero measurable
per-op cost on those paths.

## Rust

Rust wrappers add a fixed adapter layer per FFI call:

- `Result<T, ove::Error>` wrapping (constructor + return-value layout)
- `Error::from_code(rc)` decoder converting C `int` to `ove::Error`
- `Option<T>` decoding via `LvCell::try_get()` for shared state
- Bounds/null checks the compiler can't elide through the FFI boundary

The DWT-direct measurement floor (~50 ns) makes the absolute adder
visible: **typically +100–500 ns per simple FFI call, scaling to
+1 000–1 300 ns on ops where the wrapper itself does multiple FFI
hops or marshals values through `Result`**.

**Headline: median \|Δ\| 2.0–5.4% per call.  Highest of the four
bindings, consistently visible, never above 6% on the typical path.**

The fixed cost is what it is — `Result<T, E>` *is* the safety
contract of the Rust API, and disabling it isn't a binding-side
optimisation.  Three mitigations exist for tight loops:

1. **`_unchecked` variants** (used by the Rust bench's
   `time::get_us_unchecked()`) skip the Result wrap when the caller
   is willing to ignore errors — used by 2 hot paths in the bench.
2. **Inline batching**: do many ops behind a single FFI call (e.g.
   `Queue::send_many(&items)`) — not yet exposed in the public API.
3. **Acceptance**: 100–500 ns per call is below most application
   noise floors; the per-op overhead disappears in any workload
   that does I/O or any kernel work between calls.

Outliers worth knowing:

- **FreeRTOS heap `thread/create_destroy` −74%** and
  **`workqueue/create_destroy` −73%**: same static-stack-spawn
  pattern as C++.  `Thread::spawn+join` uses caller-supplied static
  memory and skips the kernel-pool round-trip.
- **FreeRTOS heap `stream/send_recv_64B` +21%** (20.6 → 25.0 µs)
  and **Zephyr heap `queue/send_receive` +27%** (4.7 → 6.0 µs): the
  Rust wrapper's `&mut [u8]` length-check + bounds-check chain adds
  ~1 000 ns on top of the underlying FFI.  Visible because both
  ops have a value-transfer body where the adapter cost compounds.
- **FreeRTOS zh `stream/send_recv_64B` −26%** (32.9 → 24.2 µs):
  *negative* delta — Rust faster than C by 8.7 µs.  This is one of
  the cases where the C wrapper's compiled flash footprint exceeds
  the Rust wrapper's on FreeRTOS zh; under cache-off the larger code
  size pays its full fetch cost.  Persistent across runs but
  cleanly the opposite sign of the same case on FreeRTOS heap.

The "Rust occasionally faster than C" rows (FreeRTOS heap
`*_create_destroy`, FreeRTOS zh `stream/send_recv_64B`,
Zephyr heap `mutex_create_destroy` −16%) all fall into one of two
buckets: different ownership semantics on construction paths
(static-stack-spawn skips kernel allocation), or compiled-code-size
asymmetry (Rust wrapper happens to be smaller than C on that op).
Neither is a wrapper "win"; just an honest accounting of the gap.

## Zig

Zig wrappers go through `comptime` trampolines that resolve to direct
FFI calls under `-OReleaseSafe`.  The pin tracker (debug-only)
compiles out, and method dispatch is monomorphised.

**Headline: median \|Δ\| 0.7–4.0% per call.  Often the tightest
binding after C, with a small set of stream-path outliers.**

Outliers worth knowing:

- **FreeRTOS heap `stream/send_recv_64B` +62%** (20.6 → 33.4 µs):
  the largest single outlier in the page.  +12.8 µs absolute on a
  20.6 µs base.  `ove.Stream.send_recv` does slice-bounds checks on
  the caller's buffer in addition to the FFI call; combined with
  Zig's compile-time-resolved alignment checks, the resulting code
  is bigger in flash than the C `ove_stream_send` + receive pair.
  *FreeRTOS zh* shows the opposite sign for the same wrapper
  (Zig 20.2 µs vs C 32.9 µs, −38%) — cleanly demonstrating that
  this is compiled-code-size + flash-fetch-ordering, not a
  binding-level fixed cost.
- **Zephyr heap `mutex_create_destroy` −31%** (11.6 → 7.9 µs):
  Zig's `Mutex.create+destroy` uses a comptime-resolved literal for
  the mutex storage, which `k_mutex_init` accepts without going
  through `k_object_alloc()`.  Same different-ownership-semantics
  pattern as Rust/C++ on the create/destroy paths.

For **per-call sync primitives** (`mutex_lock`, `sem_take`,
`event_signal`, `condvar_signal`, `recursive_mutex`) Zig stays inside
±300 ns of C on every RTOS+mode pair.  The wrapper invocation cost
is not reliably visible above the worst-case-timing noise floor on
the per-call sync paths.

## Cross-binding summary

For an op that's ~5–8 µs in C under worst-case timing (typical
`mutex_lock`, `sem_take_give`, `queue/send_receive`):

| Binding | Per-call adder | Driver |
|---|---|---|
| **C**   | 0 (baseline) | — |
| **C++** | ±0–300 ns | inlined RAII / `optional` body, mostly parity with C |
| **Rust** | +100–500 ns typical, +1 000+ ns on value-marshalling ops | `Result<T,E>` wrap, `Error::from_code`, `Option` decode |
| **Zig** | ±0–300 ns | comptime trampoline; pin tracker elided in release |

For an op that's ~50–60 µs in C (context switch, condvar, workqueue
submit) all four bindings sit within ±3% — the per-op fixed costs are
below the scheduler noise floor.

### Why Zephyr shows the biggest cross-binding spread

Zephyr's per-call sync primitives have the smallest absolute base of
the three RTOSes — `k_mutex_lock+unlock` is ~3 µs cache-off, vs
~5 µs on NuttX and ~8 µs on FreeRTOS.  The same ~200–500 ns
binding-side adder is a larger fraction of 3 µs than of 8 µs, so the
percentage |Δ| values look bigger on Zephyr without the absolute
overhead actually being larger.  The "max" 74% in Zephyr ZH C++ is a
+10 µs absolute outlier on a 14 µs base — the only genuinely large
binding-side gap on Zephyr.

### Why NuttX shows the smallest spread

NuttX's POSIX-style sync primitives are the largest of the three
(`pthread_mutex_lock+unlock` ~5 µs, `mq_*` ~12 µs).  More of the
total time is kernel work that's identical regardless of which
binding called it; the wrapper-side adder is proportionally tiny.
NuttX heap and zh both show median \|Δ\| under 4% across every
binding.

### Heap-vs-zero-heap effect per binding

| Binding | Effect of heap → ZH | Why |
|---|---|---|
| **C**   | Modest mode-dependent deltas (see [heap-vs-zeroheap](heap-vs-zeroheap.md)) | static-vs-pool allocation produces different addresses for kernel objects; flash-fetch order changes |
| **C++** | Median stays 0.8–2.8%; one mode-specific +74% outlier on Zephyr ZH stream | `optional<Stream>` wrapper compiles to a different code-size shape under ZH static storage |
| **Rust** | Stable median across modes; max grows on Zephyr heap (`queue/send_receive` +27%) | wrapper layer is mode-agnostic; the wrapper code is identical heap and ZH |
| **Zig** | `*_create_destroy` outliers gated out under ZH (per design); stream-path outliers flip sign between modes | comptime-resolved storage produces different compile-time-fixed addresses, and on cache-off paths the resulting flash-fetch ordering can flip the sign vs C |

### When to choose which binding

- **C**: lowest overhead on every config, no language-level safety.
  Choose when 100 ns per op matters and you're confident in your code.
- **C++**: clean RAII + type-safe queue at parity with C on per-call
  paths.  One mode-specific rough edge on Zephyr ZH stream.
- **Rust**: best safety guarantees; +100–500 ns fixed cost per FFI
  call (typical) under worst-case timing.  Use when correctness
  matters more than nanoseconds — most workloads.
- **Zig**: comptime safety at parity with C on per-call paths.
  Stream-path outliers flip sign by mode but never large in
  workload-relevant absolute terms.

### How to read these percentages on your hardware

The published numbers are the upper bound of per-op cost on a
Cortex-M7 platform with caches and accelerators turned off.  On your
target with caches enabled, the C-binding base op will be ~2.5–3×
smaller in wall-clock time.  The fixed wrapper adders (in absolute ns)
do not change with caches enabled, but the percentage ratio
*increases* because the denominator shrinks.  If you need a quick
estimate of the cache-on numbers, divide the C-binding base by ~2.7
and add the same fixed binding adder back; that recovers the
cache-on per-call ratio.

The deltas in this analysis are stable across runs; the hot-path
audit at `tests/audit/hotpath_expected.yaml` ensures no hidden
allocator/vtable/panic-handler ever sneaks into the measured path.
