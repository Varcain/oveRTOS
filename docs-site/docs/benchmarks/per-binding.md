# Per-language binding analysis

This page complements [Heap vs zero-heap](heap-vs-zeroheap.md) with the
orthogonal slice: for each language binding (C, C++, Rust, Zig), what's
the wrapper overhead vs the baseline C binding, and does it change
between heap and zero-heap modes?

The deltas below are the values from the **Δ \<binding\>** columns in
each per-RTOS report — wrapper-vs-native within a single hardware run,
so cross-run scheduler noise is factored out.

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
the same purpose. Without that bypass, NuttX zero-heap traps
`pthread_create`'s `kmm_zalloc(task_group_s)` with ENOMEM in
helper-thread setup and Rust+Zig benches stall in `ctx_switch_setup`.

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

| RTOS | Mode | Median |Δ| | Notable cases | Notes |
|------|------|------------|---------------|-------|
| FreeRTOS | heap     | 2.7% | `time/time_get_us_overhead` +27%, `thread/yield` −17%, `native/event_signal_wait` −11% | sub-µs op jitter floor; otherwise within ±5% on hot paths |
| FreeRTOS | zero-heap | 2.1% | `queue/send_receive` +17% | embedded-storage `Queue<T,N>` grows `std::optional<Queue<...>>`; engaged-bit check fights the buffer for a cache line |
| NuttX    | heap     | 2.5% | `eventgroup/set_get_bits` −30%, `native/sem_create_destroy` −15%, `queue/send_receive` −14%, `native/thread_yield` −14%, `sync/recursive_mutex_lock_unlock` −13%, `sync/mutex_lock_unlock` −12% | g++ register-allocation / load-hoisting wins over gcc on short sync ops — same pattern as NuttX zero-heap |
| NuttX    | zero-heap | 2.8% | `eventgroup/set_get_bits` −21%, `sync/recursive_mutex_lock_unlock` −17%, `sync/mutex_lock_unlock` −15%, `time/time_get_us_overhead` −11%, `sync/mutex_contention_2t` −11% | same g++-faster-than-gcc effect as NuttX heap; both modes consistent |
| Zephyr   | heap     | 2.5% | `sync/mutex_contention_2t` +870% (¹), `native/queue_create_destroy` −23%, `native/sem_create_destroy` −16%, `queue/send_receive` −13% | mostly slightly faster than C; one bench-design flake |
| Zephyr   | zero-heap | 4.1% | `native/sem_take_give` −13%, `sync/sem_take_give` −13%, `native/queue_create_destroy` −13%, `native/thread_create_destroy` +11% | clean — small absolute deltas |

¹ The Zephyr-heap CPP `sync/mutex_contention_2t` row at +870% (24.6 µs vs C's
2.5 µs) is the documented Zephyr-bench-design flake: with
`CONFIG_TIMESLICING=y` Zephyr round-robins same-priority threads every
1 ms, and whether the contention helper actually collides with the
runner depends on initial scheduling alignment. The wrapper code path
is identical across bindings; this row is bench-noise on Zephyr only.

**Honest read on C++**: ergonomic wins (RAII, type-safe queue) at
near-zero cost. The +17% spike on FreeRTOS zero-heap queue paths under
producer/consumer is the only non-trivial finding; it's attributable
to `std::optional<Queue<u32, 64>>`'s memory layout (the optional's
engaged bit lives next to a 256-byte queue buffer, putting buffer
accesses into a different cache line than the present-flag load).
Probably fixable by hoisting the present-bit check out of the hot
loop in the bench.

The negative deltas ("C++ faster than C") on NuttX (both heap and
zero-heap) and Zephyr (−13% to −30% on several ops) are real and
reproducible. They reflect the C++ compiler being slightly better
than C at register allocation for the specific codepath these
wrappers compile to. Not a binding win in the philosophical sense —
same FFI body — just a downstream optimizer difference.

The fact that NuttX heap and NuttX zero-heap show *the same* negative
deltas on the same suites (`eventgroup/set_get_bits` −30% vs −21%,
`mutex_lock_unlock` −12% vs −15%, `recursive_mutex_lock_unlock` −13%
vs −17%) is the strongest evidence that this is a g++-vs-gcc codegen
difference — heap-vs-zeroheap doesn't change the wrapper at all on
this side.

## Rust — large but consistent overhead

Rust wrappers add a fixed-cost adapter layer per FFI call:

- `Result<T, ove::Error>` wrapping (constructor + return-value layout)
- `Error::from_code(rc)` decoder converting C `int` to `ove::Error`
- `Option<T>` decoding via `LvCell::try_get()` for shared state
- Bounds/null checks the compiler can't elide through the FFI boundary

These add a fixed **~1.5-2 µs per call** that doesn't go away with
optimization. Whether that registers as 5% or 90% in the Δ column
depends on the underlying op's absolute latency.

| RTOS | Mode | Median Δ | Worst hot-path Δ | Notes |
|------|------|---------:|------------------|-------|
| FreeRTOS | heap     | +26.1% | `stream/throughput` +90%, `queue/throughput_2t` +70%, `time_get_us` +57% | producer/consumer doubles the per-op fixed cost |
| FreeRTOS | zero-heap | +47.3% | `queue/throughput_2t` +81%, `sync/sem_take_give` +77%, `stream/throughput` +66% | wrapper layer identical; deltas track up because C path got marginally slower under ZH |
| NuttX    | heap     |  −0.4% | `eventgroup/set_get_bits` −26%, `stream/throughput` +21%, `native/sem_create_destroy` −20%, `time_get_us` +16%, `native/sem_take_give` −11% | NuttX baseline ops are slower (3-7 µs typical) so the Rust adder is below the noise floor; sometimes Rust register allocation wins outright |
| NuttX    | zero-heap | −2.6% | `stream/throughput` +39%, `eventgroup/set_get_bits` −22%, `native/thread_yield` −20%, `native/sem_take_give` −19%, `mutex_contention_2t` −14%, `thread/yield` −13% | NuttX baseline so slow that Rust adder is sub-noise; same negative-Δ pattern as NuttX heap |
| Zephyr   | heap     | +31.7% | `stream/throughput` +74%, `native/sem_create_destroy` +64%, `native/sem_take_give` +63% | Zephyr ops are short, Rust adder visible |
| Zephyr   | zero-heap | +46.0% | `native/mutex_create_destroy` +86%, `queue/throughput_2t` +86%, `stream/throughput` +86% | identical pattern; Zephyr ZH has slightly faster C baseline so % is higher |

**Honest read on Rust**: the per-op overhead is real and not noise.
On a 3 µs mutex lock, a 1.5 µs adder is +50%. On a 23 µs context
switch or 30 µs condvar, the same 1.5 µs is +5% — invisible.

This isn't fixable without losing Rust's safety guarantees: the
`Result<T, E>` machinery *is* the safety, and it costs what it costs.
Three mitigations exist for tight loops:

1. **`_unchecked` variants** (used by the Rust bench's
   `time::get_us_unchecked()`) skip the Result wrap when the caller is
   willing to ignore errors — used by 2 hot paths in the bench, gives
   ~30% speedup on those.
2. **Inline batching**: do many ops behind a single FFI call (e.g.
   `Queue::send_many(&items)`) — not yet exposed.
3. **Acceptance**: 1.5 µs is small enough that for most workloads the
   overhead is invisible. Rust's safety win compounds across a project;
   the per-op cost doesn't.

The "Rust faster than C" pattern on NuttX (heap median Δ −0.4%,
zero-heap median Δ −2.6%) is explained by the binding's fixed adder
being a smaller fraction of the larger NuttX baseline, plus
monomorphized Rust producing slightly tighter register usage on a
few specific ops. Don't read it as "Rust is genuinely faster" — read
it as "the Rust adder is below the measurement floor for these ops
on this RTOS."

## Zig — clean, with one persistent heap-mode outlier

Zig wrappers go through comptime trampolines that resolve to direct
FFI calls under `-OReleaseSafe`. The pin tracker (debug-only) compiles
out, and method dispatch is monomorphized.

| RTOS | Mode | Median |Δ| | Notable cases | Notes |
|------|------|------------|---------------|-------|
| FreeRTOS | heap     | 3.7% | `time_get_us` +25%, `mutex_lock_unlock` +22%, `mutex_contention_2t` +15%, `recursive_mutex_lock_unlock` +11% | persistent mutex outliers in heap mode (see notes) |
| FreeRTOS | zero-heap | 2.3% | `time_get_us` +18%, `sync/sem_take_give` +14%, `queue/send_receive` +11% | mutex outliers from heap mode disappear; embedded-storage layout removes the optimizer-defeating null-check |
| NuttX    | heap     | 3.0% | `eventgroup/set_get_bits` −22%, `thread/yield` −17%, `native/thread_yield` −15%, `stream/throughput` −14%, `native/sem_create_destroy` −14%, `time_get_us` +12% | mostly negative — same NuttX-baseline-is-slow effect as Rust |
| NuttX    | zero-heap | 2.9% | `eventgroup/set_get_bits` −21%, `native/sem_take_give` −19%, `mutex_lock_unlock` −18%, `native/thread_yield` −18%, `mutex_contention_2t` −17%, `thread/yield` −13%, `time_get_us` −12% | mostly negative — same NuttX-baseline-is-slow effect as the heap mode row above |
| Zephyr   | heap     | 2.4% | `workqueue/create_destroy` +46%, `native/thread_create_destroy` +22%, `native/queue_create_destroy` −21%, `time_get_us` +17%, `sync/mutex_create_destroy` −17% | one large workqueue create outlier (heap mode only — gated under ZH) |
| Zephyr   | zero-heap | 2.4% | `native/thread_create_destroy` +25%, `queue/throughput_2t` +17%, `sync/sem_take_give` −11% | clean across the per-call hot paths |

**Honest read on Zig**: Zig is the lowest-overhead wrapper after C.
Across all 6 RTOS+mode runs, the median |Δ| sits in the 2.3-3.7%
range — close to noise floor.

Two real signals worth noting:

1. **FreeRTOS heap `mutex_lock_unlock` +22%, `mutex_contention_2t` +15%,
   `recursive_mutex_lock_unlock` +11%** — these are real,
   reproducible, and *specific to heap mode*. Under Zephyr or under
   FreeRTOS zero-heap the same ops sit within ±3% of C. Heap-mode Zig
   stores only the kernel handle, but the optimizer appears to insert
   a redundant null-check on the handle in the lock path that
   disappears once the wrapper has embedded storage (zero-heap) and
   the field layout makes the null state unreachable post-init.
   ~700 ns absolute on a 3.4 µs op — annoying but bounded.
2. **Zephyr heap `workqueue/create_destroy` +46% (58.4 µs vs C 40.0 µs)**
   — the workqueue's worker thread storage uses Zephyr-specific
   power-of-2 alignment + 128-byte FPU guard pad, which inflates the
   wrapper's stack to the next power-of-2 (4096-byte allocated for a
   2048-byte requested stack). The `_create_destroy` case spins up
   and tears down the entire workqueue + worker thread, so the larger
   stack allocation shows up. Gated out under zero-heap (the
   `*_create_destroy` cases aren't run there), so this row only
   appears in heap mode. Not present on FreeRTOS / NuttX heap because
   their backends don't have Zephyr's MPU alignment requirement.

The previously-documented Zig "FreeRTOS ZH `mutex_lock_unlock` +22%"
outlier from earlier runs is **gone** in the current data (now +0.2%),
confirming the embedded-storage refactor closed it.

## Cross-binding summary

For an op that's ~3 µs in C, here's what each binding adds:

| Binding | Typical adder | Cause | Sensitivity to heap/zh |
|---------|--------------|-------|------------------------|
| **C**   | 0 (baseline) | — | — |
| **C++** | ±100-200 ns | inlined `std::optional` body; sometimes 1 fewer load than C | small — queue paths under FreeRTOS ZH show +17% (cache layout), elsewhere ±5% |
| **Rust** | +1.5-2 µs (fixed) | `Result<T,E>` wrap, `Error::from_code`, `Option` decode in shared-state paths | none — same wrapper either mode |
| **Zig** | ±100-300 ns | comptime trampoline + (debug-only, elided) pin check | one heap-only outlier on FreeRTOS mutex paths (cleaned up by ZH) |

For an op that's ~25 µs in C (context switch, condvar, workqueue
submit), all four bindings sit within ±5% — the per-op fixed costs
are below the scheduler noise floor.

### Heap-vs-zero-heap effect per binding

| Binding | Effect of heap → zero-heap | Why |
|---------|---------------------------|-----|
| **C**   | Modest kernel-side cache shifts (see [heap-vs-zeroheap](heap-vs-zeroheap.md)) | static-vs-heap object placement |
| **C++** | Mostly stable; **queue paths +17% on FreeRTOS ZH** | embedded storage grows `std::optional<Queue<...>>` past a cache-line threshold |
| **Rust** | Stable across modes; deltas track | wrapper layer is mode-agnostic |
| **Zig** | **Improves** the FreeRTOS heap mutex outliers (`mutex_lock_unlock` +22% → +0.2%) | embedded-storage layout removes a redundant null-check the optimizer couldn't elide in the heap path |

### When to choose which binding

- **C**: lowest overhead, no language-level safety. Use when 1.5 µs per op matters and you're confident in your code.
- **C++**: clean RAII, near-C performance. Available on every RTOS+mode combo on this bench; on NuttX (heap and zero-heap) g++ codegen is reproducibly faster than gcc on short sync ops.
- **Rust**: best safety guarantees, +1.5-2 µs fixed cost per FFI call. Use when correctness matters more than nanoseconds — most workloads.
- **Zig**: comptime safety + close-to-C performance, embedded-storage wrappers benefit from zero-heap mode specifically (FreeRTOS mutex outliers disappear). Best fit for embedded with strict zero-heap discipline.

The deltas in this analysis are stable across runs; the methodology and
audit at `tests/audit/hotpath_expected.yaml` ensures no hidden
allocator/vtable/panic-handler ever sneaks into the measured hot path.
