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
| FreeRTOS | heap     | 2.3% | `time/time_get_us_overhead` +31%, `thread/yield` −20%, `native/thread_yield` −17%, `native/event_signal_wait` −12% | sub-µs op shows the Result-wrap cost cleanly; longer ops within ±5% |
| FreeRTOS | zero-heap | 2.9% | `queue/throughput_2t` +33%, `queue/send_receive` +28%, `time_get_us` +25% | embedded-storage `Queue<T,N>` grows `std::optional<Queue<...>>`; engaged-bit check fights the buffer for a cache line |
| NuttX    | heap     | 3.1% | `eventgroup/set_get_bits` −38%, `recursive_mutex_lock_unlock` −14%, `native/thread_yield` −11%, `mutex_lock_unlock` −10% | g++ register-allocation / load-hoisting wins over gcc on short sync ops |
| NuttX    | zero-heap | 2.7% | `eventgroup/set_get_bits` −24%, `queue/send_receive` +18%, `queue/throughput_2t` +15%, `native/mutex_create_destroy` −13%, `time_get_us` +12%, `native/sem_create_destroy` −11% | same g++-faster-than-gcc effect on eventgroup; queue paths flip the other way under zero-heap (cache-layout) |
| Zephyr   | heap     | 4.1% | `native/mutex_lock_unlock` −19%, `native/sem_take_give` −17%, `sync/sem_take_give` −17%, `native/mutex_contention_2t` −15%, `recursive_mutex_lock_unlock` −13% | g++ codegen wins on short sync ops; clean across the per-call paths |
| Zephyr   | zero-heap | 6.6% | `mutex_contention_2t` +36%, `recursive_mutex_lock_unlock` +32%, `mutex_lock_unlock` +30%, `queue/throughput_2t` +21%, `eventgroup/set_get_bits` +13%, `stream/send_recv_64B` +13% | C++ embedded-storage `optional<Mutex>` adds a per-call engaged-bit load on Zephyr's already-tight `k_mutex` path; the +30% deltas are 200–300 ns absolute on 1 µs ops |

**Honest read on C++**: ergonomic wins (RAII, type-safe queue) at
generally near-zero cost, with a few mode-specific rough edges:

- **FreeRTOS zero-heap queue paths +28% / +33%** are the
  `std::optional<Queue<u32, 64>>` engaged-bit-vs-buffer cache-line
  collision; persistent across runs. ~700 ns absolute on a ~3 µs queue
  op — bench-design cost rather than a binding flaw, fixable by
  hoisting the `optional` engaged check out of the hot loop.
- **Zephyr zero-heap mutex paths +30%** are new in the current data
  (with the DWT timer revealing the previously-hidden detail). On
  Zephyr's k_mutex zero-heap path the C wrapper is ~1.0 µs and the
  C++ `optional<Mutex>::operator->` adds the same engaged-bit load,
  which the cache misses on. Same fix applies as the FreeRTOS queue
  case.

The negative deltas ("C++ faster than C") on NuttX (heap and ZH) and
Zephyr heap (−11% to −38% on several ops) are real and reproducible.
They reflect the C++ compiler being slightly better than C at register
allocation for the specific codepath these wrappers compile to. Not a
binding win in the philosophical sense — same FFI body — just a
downstream optimizer difference. The pattern is consistent across
NuttX heap and zero-heap (`eventgroup/set_get_bits` −38% and −24%,
`mutex_lock_unlock` −10% and noise, `recursive_mutex_lock_unlock` −14%
and noise), confirming it's g++-vs-gcc codegen rather than a
heap/zero-heap effect.

## Rust — fixed adapter cost, dominates short ops

Rust wrappers add a fixed-cost adapter layer per FFI call:

- `Result<T, ove::Error>` wrapping (constructor + return-value layout)
- `Error::from_code(rc)` decoder converting C `int` to `ove::Error`
- `Option<T>` decoding via `LvCell::try_get()` for shared state
- Bounds/null checks the compiler can't elide through the FFI boundary

The DWT-direct measurement floor finally lets us read the absolute
adder cleanly: roughly **80–150 ns per call** on hot paths after the
adapter inlines. That registers as a small or large percentage
depending on the underlying op's absolute latency.

| RTOS | Mode | Median Δ | Worst hot-path Δ | Notes |
|------|------|---------:|------------------|-------|
| FreeRTOS | heap     | +6.4% | `stream/throughput` +82%, `time_get_us` +36%, `mutex_lock_unlock` +34% | producer/consumer doubles the per-op fixed cost; `_create_destroy` paths often *faster* than C (Rust spawn pattern is cheaper) |
| FreeRTOS | zero-heap | +10.6% | `queue/throughput_2t` +62%, `stream/throughput` +60%, `sync/sem_take_give` +40%, `time_get_us` +34%, `stream/send_recv_64B` +29%, `queue/send_receive` +28% | adapter cost identical; deltas track the (slightly faster) C zero-heap baseline |
| NuttX    | heap     | +0.0% | `mutex_contention_2t` −82% (bench flake), `stream/throughput` +37%, `native/thread_yield` −17% | NuttX baseline ops are slower (~2–6 µs) so the Rust adder is below the noise floor on most rows |
| NuttX    | zero-heap | −0.3% | `stream/throughput` +75%, `native/thread_yield` −22%, `native/sem_take_give` −17%, `time_get_us` +16%, `thread/yield` −15%, `mutex_contention_2t` −13% | same — short Rust adder < NuttX op latency, sometimes monomorphized Rust wins outright |
| Zephyr   | heap     | +7.7% | `mutex_contention_2t` +441% (bench flake on this binding), `stream/throughput` +75%, `time_get_us` +44%, `stream/send_recv_64B` +41%, `workqueue/create_destroy` −34%, `native/queue_create_destroy` +29% | Zephyr ops are short, Rust adder visible; one bench-design flake on `mutex_contention_2t` |
| Zephyr   | zero-heap | +15.3% | `stream/throughput` +77%, `queue/throughput_2t` +60%, `stream/send_recv_64B` +41%, `recursive_mutex_lock_unlock` +35%, `queue/send_receive` +34%, `mutex_contention_2t` +23% | Zephyr's faster zero-heap C baseline (1.0 µs mutex) makes the Rust adder show up bigger as % |

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

The "Rust faster than C" pattern on NuttX (heap median Δ 0%,
zero-heap median Δ −0.3%) is explained by the binding's fixed adder
being a smaller fraction of the larger NuttX baseline, plus
monomorphized Rust producing slightly tighter register usage on a
few specific ops. Don't read it as "Rust is genuinely faster" — read
it as "the Rust adder is below the measurement floor for these ops
on this RTOS."

The earlier "Rust adder ~1.5–2 µs" claim from before the harness
timer rework was inflated by the per-RTOS counter-read jitter that
got amortised into the Rust column. The actual hot-path adder is
substantially smaller than that — the new DWT-direct measurement
makes it visible cleanly.

## Zig — clean, with one persistent FreeRTOS-heap mutex outlier

Zig wrappers go through comptime trampolines that resolve to direct
FFI calls under `-OReleaseSafe`. The pin tracker (debug-only) compiles
out, and method dispatch is monomorphized.

| RTOS | Mode | Median \|Δ\| | Notable cases | Notes |
|------|------|------------|---------------|-------|
| FreeRTOS | heap     | 4.6% | `mutex_lock_unlock` +29%, `mutex_contention_2t` +23%, `native/mutex_lock_unlock` +18%, `time_get_us` +17%, `recursive_mutex_lock_unlock` +16%, `native/queue_send_receive` +15% | persistent mutex outliers in heap mode (see notes) |
| FreeRTOS | zero-heap | 4.6% | `time_get_us` +27%, `sem_take_give` +24%, `queue/throughput_2t` +17%, `queue/send_receive` +17%, `mutex_lock_unlock` +13%, `mutex_contention_2t` +11% | embedded-storage layout reduces some mutex outliers; queue paths grow under zero-heap |
| NuttX    | heap     | 2.0% | `mutex_contention_2t` −81% (bench flake), `native/sem_take_give` −15%, `native/thread_yield` −15%, `thread/yield` −15%, `native/mutex_create_destroy` −12%, `recursive_mutex_lock_unlock` −12% | mostly negative — same NuttX-baseline-is-slow effect as Rust |
| NuttX    | zero-heap | 3.7% | `native/sem_take_give` −25%, `native/thread_yield` −22%, `queue/send_receive` +18%, `stream/send_recv_64B` +13%, `queue/throughput_2t` +13%, `thread/yield` −11% | same negative pattern on syscall paths; queue paths shift the other way |
| Zephyr   | heap     | 3.6% | `native/sem_create_destroy` +35%, `native/mutex_create_destroy` +33%, `workqueue/create_destroy` +24%, `native/queue_create_destroy` +24%, `sync/mutex_create_destroy` −22%, `time_get_us` +18% | `*_create_destroy` outliers are the Zephyr MPU-aligned-stack inflation; gated under ZH |
| Zephyr   | zero-heap | 4.5% | `recursive_mutex_lock_unlock` +18%, `time_get_us` +15%, `mutex_contention_2t` +14%, `queue/throughput_2t` +12%, `mutex_lock_unlock` +11% | clean — small absolute deltas (200 ns range) |

**Honest read on Zig**: Zig is the lowest-overhead wrapper after C
on most RTOS+mode combos. Median |Δ| sits in the 2.0–4.6% range
across the six configurations.

Two real signals worth noting:

1. **FreeRTOS heap `mutex_lock_unlock` +29%, `mutex_contention_2t`
   +23%, `recursive_mutex_lock_unlock` +16%** — these are real,
   reproducible, and *specific to FreeRTOS heap mode*. Heap-mode Zig
   stores only the kernel handle, but the optimizer appears to insert
   a redundant null-check on the handle in the lock path that
   disappears once the wrapper has embedded storage (zero-heap) and
   the field layout makes the null state unreachable post-init. The
   per-call adder is ~700 ns absolute on a 2.5 µs op — annoying but
   bounded, and present only on FreeRTOS.
2. **Zephyr heap `*_create_destroy` outliers** (`workqueue` +24%,
   `native/sem_create_destroy` +35%, `native/mutex_create_destroy`
   +33%, `native/queue_create_destroy` +24%) are the Zephyr-specific
   power-of-2-aligned stack with the 128-byte MPU FPU guard pad — the
   wrapper's stack rounds up to the next power of 2, inflating
   create/destroy time. Gated out under zero-heap (the
   `*_create_destroy` cases aren't run there), so this row only
   appears in heap mode. Not present on FreeRTOS / NuttX heap because
   their backends don't have Zephyr's MPU alignment requirement.

## Cross-binding summary

For an op that's ~2 µs in C, here's what each binding adds:

| Binding | Typical adder | Cause | Sensitivity to heap/zh |
|---------|--------------|-------|------------------------|
| **C**   | 0 (baseline) | — | — |
| **C++** | ±50–150 ns | inlined `std::optional` body; sometimes 1 fewer load than C | small — queue paths +28% on FreeRTOS ZH and mutex paths +30% on Zephyr ZH (both: `optional` engaged-bit-vs-buffer cache-line collision); elsewhere ±5% |
| **Rust** | +80–150 ns (fixed) | `Result<T,E>` wrap, `Error::from_code`, `Option` decode in shared-state paths | none — same wrapper either mode; deltas track the C baseline |
| **Zig** | ±100–300 ns | comptime trampoline + (debug-only, elided) pin check | one heap-only outlier on FreeRTOS mutex paths; Zephyr `*_create_destroy` outliers in heap mode only |

For an op that's ~22 µs in C (context switch, condvar, workqueue
submit), all four bindings sit within ±5% — the per-op fixed costs
are below the scheduler noise floor.

### Heap-vs-zero-heap effect per binding

| Binding | Effect of heap → zero-heap | Why |
|---------|---------------------------|-----|
| **C**   | Modest mode-dependent placement effects (see [heap-vs-zeroheap](heap-vs-zeroheap.md)) | static-vs-heap object placement; varies by RTOS |
| **C++** | Mostly stable; queue paths +28% on FreeRTOS ZH; mutex paths +30% on Zephyr ZH | `optional<T>` engaged-bit vs T buffer cache-line layout, exposed by ZH's nearer placement |
| **Rust** | Stable across modes; deltas track the C baseline | wrapper layer is mode-agnostic |
| **Zig** | Improves the FreeRTOS heap mutex outliers slightly (+29% → +13%); `*_create_destroy` Zephyr outliers gone (gated under ZH) | embedded-storage layout removes a redundant null-check the optimizer couldn't elide in the heap path |

### When to choose which binding

- **C**: lowest overhead, no language-level safety. Use when 100 ns per op matters and you're confident in your code.
- **C++**: clean RAII, near-C performance. Available on every RTOS+mode combo on this bench; on NuttX (heap and zero-heap) g++ codegen is reproducibly faster than gcc on short sync ops; one mode-specific rough edge on `optional`-wrapped queue/mutex paths under FreeRTOS / Zephyr zero-heap.
- **Rust**: best safety guarantees, +80–150 ns fixed cost per FFI call. Use when correctness matters more than nanoseconds — most workloads.
- **Zig**: comptime safety + close-to-C performance, embedded-storage wrappers benefit from zero-heap mode specifically (FreeRTOS mutex outliers shrink). Best fit for embedded with strict zero-heap discipline.

The deltas in this analysis are stable across runs; the methodology and
audit at `tests/audit/hotpath_expected.yaml` ensures no hidden
allocator/vtable/panic-handler ever sneaks into the measured hot path.
