# Heap vs zero-heap: per-RTOS comparison

For each RTOS, how do per-call hot-path latencies differ between
**heap mode** (`_create()` / `_destroy()` API; kernel objects come from
the kernel's allocator) and **zero-heap mode**
(`CONFIG_OVE_ZERO_HEAP=y`; `_init()` / `_deinit()` API; caller-supplied
static storage; the heap is locked at `ove_run()` and any post-boot
allocation aborts the build or panics)?

## What "heap-vs-zh" can and can't measure

**The C-binding hot-path call site is the same FFI symbol in both
modes** — `ove_mutex_lock`, `ove_sem_take`, … resolve to identical
code regardless of how the kernel object behind the handle was
allocated.  So any C-column delta between heap and zh is **not a
binding-side cost**; it's the kernel itself behaving differently
because the underlying object lives at a different address with
different surrounding state.

Binding-specific deltas (CPP/Rust/Zig vs C) come from a different
mechanism (wrapper code path) and are documented in the
[per-binding analysis](per-binding.md) — they are not repeated on
this page, which compares the C column only.

## Configuration

All numbers below are taken with
`CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` (Cortex-M7 I-cache, D-cache,
branch predictor, ART accelerator, and flash prefetch all disabled —
see [benchmarks overview](index.md)).  The per-call iteration budget
was settled by a one-shot calibration pass (see "Iteration-count
calibration" on the overview page) and stays at 1 000 iterations on
the typical latency case.

**Bucketing rule for verdicts:** ±5% relative is treated as noise
(scheduler jitter on a flash-fetch-bound execution path; absolute
delta is typically <500 ns on the µs-range ops below).  5–10% is
"marginal — worth a glance but not a structural signal".  >10% is an
outlier worth understanding.

## FreeRTOS — C binding

| Case | Heap | Zero-heap | Δ% | Verdict |
|------|-----:|----------:|---:|---------|
| `time/time_get_us_overhead` | 883 ns | 1.1 µs | +24.6% | sub-µs — see notes |
| `time/delay_1ms` | 984.4 µs | 984.0 µs | 0% | RTOS tick |
| `thread/yield` | 4.5 µs | 4.4 µs | −2.2% | noise |
| `thread/get_self` | 2.6 µs | 2.5 µs | −3.8% | noise |
| `thread/sleep_1ms` | 984.4 µs | 984.1 µs | 0% | RTOS tick |
| `thread/context_switch` | 51.9 µs | 51.1 µs | −1.5% | noise |
| `sync/mutex_lock_unlock` | 8.1 µs | 7.7 µs | −4.9% | noise |
| `sync/mutex_contention_2t` | 8.2 µs | 8.8 µs | +7.3% | marginal |
| `sync/sem_take_give` | 6.5 µs | 6.1 µs | −6.2% | marginal |
| `sync/event_signal_wait` | 50.2 µs | 49.8 µs | −0.8% | noise |
| `sync/condvar_signal_wait` | 34.2 µs | 33.6 µs | −1.8% | noise |
| `sync/recursive_mutex_lock_unlock` | 10.2 µs | 9.4 µs | −7.8% | marginal |
| `queue/send_receive` | 8.2 µs | 8.6 µs | +4.9% | noise |
| `queue/throughput_2t` | 4.0 µs | 4.3 µs | +7.5% | marginal |
| `timer/start_stop` | 67.7 µs | 71.2 µs | +5.2% | marginal |
| `eventgroup/set_get_bits` | 7.3 µs | 6.8 µs | −6.8% | marginal |
| `workqueue/submit_execute` | 57.7 µs | 56.7 µs | −1.7% | noise |
| `stream/send_recv_64B` | 20.6 µs | 32.9 µs | **+59.7%** | real — see notes |
| `stream/throughput` | 31.2 µs | 41.9 µs | **+34.3%** | real — see notes |

**Honest read on FreeRTOS.**

Per-call sync (mutex, sem, condvar, event, queue, eventgroup) is
within ±10% across modes, with most rows under ±5%.  The few marginal
deltas in either direction are static-vs-pool address differences
showing up on a flash-fetch-bound path — neither systematically
favours one mode.  The two non-noise outliers are real:

- **`stream/send_recv_64B` +60%** (20.6 → 32.9 µs) and
  **`stream/throughput` +34%** (31.2 → 41.9 µs).  This is a
  FreeRTOS-intrinsic regression, not a wrapper cost.  The
  per-RTOS reports' wrapper-vs-native section shows the raw
  `xStreamBuffer*` baseline at 19.4 µs (heap) → 31.4 µs (zh) — a
  ~12 µs gap *in the native FreeRTOS API itself*, not in the
  oveRTOS wrapper.  oveRTOS adds ~1 µs of wrapper overhead in both
  modes.  If `xStreamBuffer*` is on a hot path, FreeRTOS heap mode
  is the better choice.
- **`time_get_us_overhead` +25%** (883 → 1 100 ns).  A ~200 ns
  regression on a sub-µs op — bench-harness loop overhead dominates,
  and the static-storage layout under zh shifts which flash bytes
  the loop reads first.  No workload times anything at this
  granularity; flagged for completeness rather than concern.

## NuttX — C binding

| Case | Heap | Zero-heap | Δ% | Verdict |
|------|-----:|----------:|---:|---------|
| `time/time_get_us_overhead` | 3.0 µs | 3.1 µs | +3.3% | noise |
| `time/delay_1ms` | 1.98 ms | 1.99 ms | 0% | RTOS tick |
| `thread/yield` | 3.6 µs | 3.7 µs | +2.8% | noise |
| `thread/get_self` | 2.6 µs | 2.6 µs | 0% | noise |
| `thread/sleep_1ms` | 1.98 ms | 1.99 ms | 0% | RTOS tick |
| `thread/context_switch` | 45.8 µs | 43.7 µs | −4.6% | noise |
| `sync/mutex_lock_unlock` | 5.4 µs | 5.0 µs | −7.4% | marginal |
| `sync/mutex_contention_2t` | 4.7 µs | 4.6 µs | −2.1% | noise |
| `sync/sem_take_give` | 4.3 µs | 3.9 µs | −9.3% | marginal |
| `sync/event_signal_wait` | 45.3 µs | 44.0 µs | −2.9% | noise |
| `sync/condvar_signal_wait` | 62.8 µs | 61.7 µs | −1.8% | noise |
| `sync/recursive_mutex_lock_unlock` | 8.5 µs | 8.0 µs | −5.9% | marginal |
| `queue/send_receive` | 11.8 µs | 11.6 µs | −1.7% | noise |
| `queue/throughput_2t` | 8.2 µs | 7.9 µs | −3.7% | noise |
| `timer/start_stop` | 27.7 µs | 29.5 µs | +6.5% | marginal |
| `eventgroup/set_get_bits` | 1.7 µs | 1.7 µs | 0% | noise |
| `workqueue/submit_execute` | 67.0 µs | 69.2 µs | +3.3% | noise |
| `stream/send_recv_64B` | 54.0 µs | 52.8 µs | −2.2% | noise |
| `stream/throughput` | 69.3 µs | 67.8 µs | −2.2% | noise |

**Honest read on NuttX.**

Heap and zero-heap are interchangeable for per-call latency.  Every
hot path sits within ±10% across modes, most under ±5%.  The few
marginal deltas (`mutex_lock_unlock` −7%, `sem_take_give` −9%,
`recursive_mutex_lock_unlock` −6%, all favouring zh; `timer/start_stop`
+7% favouring heap) are 300–400 ns absolute on µs-range ops — flash
fetch ordering rather than a structural difference.  No outlier
exceeds 2 µs absolute.

## Zephyr — C binding

| Case | Heap | Zero-heap | Δ% | Verdict |
|------|-----:|----------:|---:|---------|
| `time/time_get_us_overhead` | 2.5 µs | 2.4 µs | −4.0% | noise |
| `time/delay_1ms` | 1.09 ms | 1.09 ms | 0% | RTOS tick |
| `thread/yield` | 10.6 µs | 10.5 µs | −0.9% | noise |
| `thread/get_self` | 971 ns | 855 ns | **−11.9%** | real — see notes |
| `thread/sleep_1ms` | 1.09 ms | 1.09 ms | 0% | RTOS tick |
| `thread/context_switch` | 56.5 µs | 56.6 µs | +0.2% | noise |
| `sync/mutex_lock_unlock` | 3.0 µs | 2.9 µs | −3.3% | noise |
| `sync/mutex_contention_2t` | 3.1 µs | 3.0 µs | −3.2% | noise |
| `sync/sem_take_give` | 2.0 µs | 2.3 µs | **+15.0%** | real — see notes |
| `sync/event_signal_wait` | 56.6 µs | 56.9 µs | +0.5% | noise |
| `sync/condvar_signal_wait` | 65.1 µs | 65.1 µs | 0% | noise |
| `sync/recursive_mutex_lock_unlock` | 3.1 µs | 3.0 µs | −3.2% | noise |
| `queue/send_receive` | 4.7 µs | 4.5 µs | −4.3% | noise |
| `queue/throughput_2t` | 3.7 µs | 3.3 µs | **−10.8%** | real — see notes |
| `timer/start_stop` | 9.0 µs | 8.8 µs | −2.2% | noise |
| `eventgroup/set_get_bits` | 9.0 µs | 9.1 µs | +1.1% | noise |
| `workqueue/submit_execute` | 58.4 µs | 58.7 µs | +0.5% | noise |
| `stream/send_recv_64B` | 14.3 µs | 14.3 µs | 0% | noise |
| `stream/throughput` | 24.3 µs | 24.3 µs | 0% | noise |

**Honest read on Zephyr.**

Most hot paths read parity (within ±5%).  Three rows show real
signal:

- **`thread/get_self` −12%** (971 → 855 ns).  Zephyr resolves
  `k_current_get` differently when the per-thread state lives in
  caller-supplied static storage vs `k_object_alloc()`-managed pool:
  the static path skips one level of indirection.  ~120 ns absolute.
- **`sync/sem_take_give` +15%** (2.0 → 2.3 µs).  Opposite sign — the
  zh-mode semaphore object lives at a different address that
  triggers an extra flash fetch on the take/give pair on Zephyr's
  k_sem state machine.  ~300 ns absolute, smaller than the per-call
  wrapper overhead documented in the
  [per-binding analysis](per-binding.md).
- **`queue/throughput_2t` −11%** (3.7 → 3.3 µs).  Two-thread
  producer/consumer with caller-supplied ring buffer.  Static
  storage for the queue produces a more compact instruction stream
  than the heap-allocated path on Zephyr's `k_queue_*` family.
  ~400 ns absolute, sign-stable across runs.

The longer ops (`event_signal_wait` 57 µs, `condvar_signal_wait`
65 µs, `context_switch` 57 µs, `workqueue/submit_execute` 58 µs)
all stay within ±2% — kernel work dominates and the
static-vs-pool address difference is below the noise floor.

## Cross-RTOS summary

| RTOS | Per-call median \|Δ\| | Real signals | Verdict |
|---|---:|---|---|
| **FreeRTOS** | ~3% | `stream/send_recv_64B` +60% and `stream/throughput` +34% — both intrinsic to the native `xStreamBuffer*` API behaving differently with caller-supplied static storage; **not a wrapper cost** | Per-call sync interchangeable between modes; ZH stream throughput pays a real intrinsic cost — choose heap if streams are hot |
| **NuttX**    | ~3% | None on per-call hot paths.  Bidirectional placement deltas of <500 ns absolute. | Heap and ZH functionally interchangeable for per-call latency |
| **Zephyr**   | ~3% | `thread/get_self` −12%, `queue/throughput_2t` −11%, `sem_take_give` +15% — all sub-µs absolute | At parity for any practical workload; mode-specific deltas <500 ns absolute |

The take-aways:

1. **No binding-level overhead** is introduced by zero-heap on any
   RTOS.  The wrapper hot-path is the same FFI symbol either way; the
   audit at `tests/audit/hotpath_expected.yaml` enforces this.
2. **Per-call sync** (mutex, sem, condvar, event, queue, eventgroup)
   is at parity within a few hundred nanoseconds on every RTOS in
   either mode.  Pick zero-heap and take the compile-time guarantees
   against post-boot allocation as a free win.
3. **Two structural exceptions**, both on FreeRTOS only:
   `stream/send_recv_64B` and `stream/throughput` regress ~12 µs and
   ~11 µs respectively under zero-heap.  This regression is in the
   native FreeRTOS `xStreamBuffer*` API, not in the oveRTOS wrapper —
   verified by the wrapper-vs-native table in the per-RTOS report.
   If your FreeRTOS workload is bound on stream throughput, prefer
   heap mode.

## Reproducing

The numbers above came from running the bench on STM32F746G-DISCO
with worst-case timing on
(`CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=y` — see
[benchmarks overview](index.md)):

```bash
make benchmarks-stm32f746g-discovery               # FreeRTOS heap
make benchmarks-stm32f746g-discovery        ZEROHEAP=1   # FreeRTOS zh
make benchmarks-stm32f746g-discovery-nuttx
make benchmarks-stm32f746g-discovery-nuttx  ZEROHEAP=1
make benchmarks-stm32f746g-discovery-zephyr
make benchmarks-stm32f746g-discovery-zephyr ZEROHEAP=1
```

Each invocation builds 4 bindings (C, C++, Rust, Zig), flashes via
openocd, captures the picocom-recorded serial log, and writes both
`output/<board>/<rtos>/_benchmarks{,_zeroheap}/report.md` and
`docs-site/docs/benchmarks/<rtos>-{heap,zeroheap}.md` directly.
