# Per-language binding analysis

This page complements [Heap vs zero-heap](heap-vs-zeroheap.md) with the
orthogonal slice: for each language binding (C, C++, Rust, Zig), what's
the wrapper overhead vs the baseline C binding, and does it change
between heap and zero-heap modes?

The deltas below are the values from the **Δ \<binding\>** columns in
each per-RTOS report — wrapper-vs-native within a single hardware run,
so cross-run scheduler noise is factored out. Bench-harness timing on
STM32F7 reads the ARMv7-M DWT cycle counter directly (one volatile
load) — uniform across all three RTOSes, so the numbers below are
not contaminated by per-RTOS timer-read overhead.

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
would block on NuttX. The C/CPP bench has always done this; Rust
exposes `ove::start_scheduler()` and Zig `ove.startScheduler()` for
the same purpose.

## C — the baseline

C is the reference (Δ = 0% by definition). Every wrapper above this
adds either:

1. A function-call frame (Rust/C++ if the wrapper isn't inlined)
2. An error/Result conversion
3. A handle-validity check
4. A trampoline for callbacks (Zig/C++)

How much of each shows up depends on optimizer behavior, ABI, and
inlining decisions. The hot-path audit at
`tests/audit/hotpath_expected.yaml` enforces that no *unexpected*
callees appear (allocators, vtables, panic handlers); what's left is
what's measured below.

## C++ — generally clean, sometimes faster than C

C++ uses RAII wrappers + `std::optional` for setup/teardown lifecycle.
Methods are header-only and inline cleanly under `-O2`.

| RTOS | Mode | Median \|Δ\| | Notable cases | Notes |
|------|------|------------|---------------|-------|
| FreeRTOS | heap     | 2.7% | `thread/yield` −17%, `eventgroup/set_get_bits` +12%, `native/stream_send_recv_64B` +12%, `native/thread_yield` −9% | within ±5% on most paths; small wins on yield-style ops, small losses on bursty paths |
| FreeRTOS | zero-heap | 2.7% | `queue/throughput_2t` +19%, `eventgroup/set_get_bits` −13%, `native/sem_take_give` +11%, `mutex_contention_2t` +11% | embedded-storage layout exposes the `optional` engaged check on shorter ops |
| NuttX    | heap     | 1.5% | `thread/get_self` −34%, `native/thread_yield` +13%, `thread/yield` +11%, `native/sem_take_give` −7% | g++ codegen wins on accessor paths; tightest binding on NuttX |
| NuttX    | zero-heap | 4.0% | `thread/yield` −28%, `queue/throughput_2t` +25%, `native/mutex_create_destroy` −22%, `queue/send_receive` +22% | mixed signs; `thread/yield` benefits from g++'s tighter codegen on the shorter NuttX path |
| Zephyr   | heap     | 2.4% | `queue/send_receive` +15%, `mutex_contention_2t` +14%, `native/queue_send_receive` +13%, `timer/start_stop` +10% | clean across the per-call paths |
| Zephyr   | zero-heap | 4.4% | `mutex_contention_2t` +40%, `recursive_mutex_lock_unlock` +38%, `mutex_lock_unlock` +30%, `queue/throughput_2t` +19% | embedded-storage `optional<Mutex>` adds engaged-bit load on Zephyr's already-tight 1 µs `k_mutex`; +30% is ~300 ns absolute |

**Honest read on C++**: ergonomic wins (RAII, type-safe queue) at
generally near-zero cost, with a few mode-specific rough edges:

- **FreeRTOS zero-heap short sync paths +22–25%** are the
  `std::optional<Mutex/Queue>` engaged-bit-vs-buffer cache-line
  collision; persistent across runs. ~400–600 ns absolute on 2–3 µs
  ops — bench-design cost rather than a binding flaw, fixable by
  hoisting the `optional` engaged check out of the hot loop.
- **Zephyr zero-heap mutex paths +30%**: on Zephyr's k_mutex zero-heap
  path the C wrapper is ~1.1 µs and the C++ `optional<Mutex>::operator->`
  adds the same engaged-bit load, which the cache misses on. Same fix
  applies as the FreeRTOS case.

The negative deltas ("C++ faster than C") on NuttX (heap and ZH) and
Zephyr heap (−11% to −35% on several ops) are real and reproducible.
They reflect the C++ compiler being slightly better than C at register
allocation for the specific codepath these wrappers compile to. Not a
binding win in the philosophical sense — same FFI body — just a
downstream optimizer difference.

## Rust — fixed adapter cost, dominates short ops

Rust wrappers add a fixed-cost adapter layer per FFI call:

- `Result<T, ove::Error>` wrapping (constructor + return-value layout)
- `Error::from_code(rc)` decoder converting C `int` to `ove::Error`
- `Option<T>` decoding via `LvCell::try_get()` for shared state
- Bounds/null checks the compiler can't elide through the FFI boundary

The DWT-direct measurement floor (single LDR from `0xE0001004`,
~50 ns floor) reveals the absolute adder cleanly: roughly
**80–150 ns per call** on hot paths after the adapter inlines.
That registers as a small or large percentage depending on the
underlying op's absolute latency.

| RTOS | Mode | Median \|Δ\| | Worst hot-path Δ | Notes |
|------|------|---------:|------------------|-------|
| FreeRTOS | heap     | 14.3% | `stream/throughput` +67%, `native/thread_create_destroy` −56%, `thread/create_destroy` −47% | producer/consumer doubles the per-op fixed cost on stream paths |
| FreeRTOS | zero-heap | 10.4% | `queue/throughput_2t` +72%, `stream/throughput` +70%, `stream/send_recv_64B` +44%, `queue/send_receive` +42% | embedded-storage cache layout pushes Rust adder visibility on shorter paths |
| NuttX    | heap     | 3.8% | `stream/throughput` +30%, `thread/get_self` −30%, `native/sem_create_destroy` +20%, `mutex_contention_2t` −17% | NuttX baseline ops slower (~2–6 µs), Rust adder below noise on most rows |
| NuttX    | zero-heap | 6.4% | `stream/throughput` +42%, `sem_take_give` +20%, `native/sem_create_destroy` +19%, `queue/send_receive` +16% | Rust adapter visible on the few short NuttX zh paths |
| Zephyr   | heap     | 9.5% | `stream/throughput` +60%, `thread/create_destroy` −48%, `queue/throughput_2t` +33%, `timer/start_stop` +29% | Zephyr ops are short, Rust adder visible; spawn paths benefit from Rust's pattern |
| Zephyr   | zero-heap | 12.8% | `stream/throughput` +68%, `queue/throughput_2t` +60%, `queue/send_receive` +45%, `mutex_contention_2t` +30% | Zephyr's tight zero-heap C baseline (1.1 µs mutex) makes the Rust adder show as bigger % |

**Honest read on Rust**: the per-op overhead is real and not noise.
On a 1 µs Zephyr mutex lock, an 80 ns adder is +8% but the Rust path
adds Result-wrap + null-check + Error::from_code which lands closer
to +150 ns and shows as +20–35% on the tightest paths. On a 22 µs
context switch the same adder is +0.5% — invisible.

This isn't fixable without losing Rust's safety guarantees: the
`Result<T, E>` machinery *is* the safety, and it costs what it costs.
Three mitigations exist for tight loops:

1. **`_unchecked` variants** (used by the Rust bench's
   `time::get_us_unchecked()`) skip the Result wrap when the caller is
   willing to ignore errors — used by 2 hot paths in the bench, gives
   ~30% speedup on those.
2. **Inline batching**: do many ops behind a single FFI call (e.g.
   `Queue::send_many(&items)`) — not yet exposed.
3. **Acceptance**: 80–150 ns per call is small enough that for most
   workloads the overhead is invisible. Rust's safety win compounds
   across a project; the per-op cost doesn't.

The "Rust occasionally faster than C" pattern (e.g. NuttX
`thread/get_self` −30%, FreeRTOS `thread/create_destroy` −47%) is
explained by the binding's fixed adder being a smaller fraction of
the larger NuttX/spawn baseline, plus monomorphized Rust producing
slightly tighter register usage on a few specific ops. Don't read
it as "Rust is genuinely faster" — read it as "the Rust adder is
below the measurement floor for these ops on this RTOS."

## Zig — clean, with one persistent FreeRTOS-heap mutex outlier

Zig wrappers go through comptime trampolines that resolve to direct
FFI calls under `-OReleaseSafe`. The pin tracker (debug-only) compiles
out, and method dispatch is monomorphized.

| RTOS | Mode | Median \|Δ\| | Notable cases | Notes |
|------|------|------------|---------------|-------|
| FreeRTOS | heap     | 4.3% | `thread/yield` −17%, `eventgroup/set_get_bits` +16%, `native/stream_send_recv_64B` +14%, `mutex_contention_2t` +13% | clean on per-call paths; sub-µs deltas on µs-scale ops |
| FreeRTOS | zero-heap | 3.4% | `sem_take_give` +19%, `thread/yield` −18%, `queue/throughput_2t` +16%, `recursive_mutex_lock_unlock` −7% | embedded-storage layout exposes the wrapper invocation on shorter ops |
| NuttX    | heap     | 2.0% | `native/sem_create_destroy` +47%, `thread/get_self` −47%, `time_get_us` +24%, `mutex_contention_2t` −18% | mostly within ±5%; spawn-path outliers absorbed by NuttX baseline cost |
| NuttX    | zero-heap | 5.0% | `native/sem_create_destroy` +23%, `queue/send_receive` +19%, `native/mutex_lock_unlock` −14%, `queue/throughput_2t` +13% | clean; outliers within 200–400 ns absolute on µs-range ops |
| Zephyr   | heap     | 5.7% | `native/mutex_contention_2t` +85%, `native/mutex_lock_unlock` −26%, `native/queue_send_receive` +24%, `queue/throughput_2t` +22% | bimodal `native_mutex_contention_2t` outlier; per-call paths clean |
| Zephyr   | zero-heap | 2.6% | `mutex_contention_2t` +34%, `queue/send_receive` +30%, `queue/throughput_2t` +26%, `recursive_mutex_lock_unlock` +14% | tightest binding in zero-heap mode; ~200 ns absolute on Zephyr's 1 µs paths |

**Honest read on Zig**: Zig is the lowest-overhead wrapper after C
on most RTOS+mode combos. Median |Δ| sits in the 2.0–6.6% range
across the six configurations.

Two real signals worth noting:

1. **FreeRTOS heap `*_create_destroy` outliers (+30–50%)**: the Zig
   `Thread<N>` spawn helper composes a per-iteration trampoline that
   touches more cache lines than the C `ove_thread_create()` direct
   call. ~9 µs absolute on a 25–47 µs spawn — visible as a large
   percentage but bounded, and not on the per-call hot path (gated
   out under zero-heap).
2. **Zephyr zero-heap short sync paths +10–25%**: on Zephyr's fast
   1 µs `k_mutex` lock, Zig's tracker-cleared / wrapper invocation
   adds ~150–250 ns. Persistent across runs but small in absolute
   terms. The same wrappers are inside ±5% on every other RTOS+mode
   pair, confirming this is Zephyr-baseline-is-fast rather than a
   binding regression.

## Cross-binding summary

For an op that's ~2 µs in C, here's what each binding adds:

| Binding | Typical adder | Cause | Sensitivity to heap/zh |
|---------|--------------|-------|------------------------|
| **C**   | 0 (baseline) | — | — |
| **C++** | ±50–150 ns | inlined `std::optional` body; sometimes 1 fewer load than C | small — queue/sync paths +20–30% on FreeRTOS ZH, mutex paths +30% on Zephyr ZH (both: `optional` engaged-bit-vs-buffer cache-line collision); elsewhere ±5% |
| **Rust** | +80–150 ns (fixed) | `Result<T,E>` wrap, `Error::from_code`, `Option` decode in shared-state paths | none — same wrapper either mode; deltas track the C baseline (visible as +30–60% on µs-scale paths because the absolute adder is ~200 ns) |
| **Zig** | ±100–300 ns | comptime trampoline + (debug-only, elided) pin check | spawn-path outliers on FreeRTOS heap (`*_create_destroy` gated out under ZH); short sync paths +10–25% on Zephyr ZH from Zig wrapper invocation cost |

For an op that's ~22 µs in C (context switch, condvar, workqueue
submit), all four bindings sit within ±5% — the per-op fixed costs
are below the scheduler noise floor.

### Heap-vs-zero-heap effect per binding

| Binding | Effect of heap → zero-heap | Why |
|---------|---------------------------|-----|
| **C**   | Modest mode-dependent placement effects (see [heap-vs-zeroheap](heap-vs-zeroheap.md)) | static-vs-heap object placement; varies by RTOS |
| **C++** | Median |Δ| stays small (1–5%); a handful of cache-collision outliers (FreeRTOS ZH queue/sync, Zephyr ZH mutex) | `optional<T>` engaged-bit vs T buffer cache-line layout, exposed by ZH's nearer placement |
| **Rust** | Stable median across modes; deltas track the C baseline | wrapper layer is mode-agnostic |
| **Zig** | `*_create_destroy` outliers gated out under ZH (per design); short sync paths +10–25% on Zephyr ZH | embedded-storage layout near the bench's working set lets the wrapper invocation cost show through |

### When to choose which binding

- **C**: lowest overhead, no language-level safety. Use when 100 ns per op matters and you're confident in your code.
- **C++**: clean RAII, near-C performance. Available on every RTOS+mode combo on this bench; on NuttX (heap and zero-heap) g++ codegen is reproducibly faster than gcc on short sync ops; one mode-specific rough edge on `optional`-wrapped queue/mutex paths under FreeRTOS / Zephyr zero-heap.
- **Rust**: best safety guarantees, +80–150 ns fixed cost per FFI call. Use when correctness matters more than nanoseconds — most workloads.
- **Zig**: comptime safety + close-to-C performance. The wrapper compiles to a clean monomorphized FFI on every RTOS+mode pair; the ~200 ns wrapper invocation cost is visible only on Zephyr's already-tight 1 µs sync paths.

The deltas in this analysis are stable across runs; the methodology and
audit at `tests/audit/hotpath_expected.yaml` ensures no hidden
allocator/vtable/panic-handler ever sneaks into the measured hot path.
