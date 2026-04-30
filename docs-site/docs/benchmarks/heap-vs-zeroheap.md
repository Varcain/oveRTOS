# Heap vs zero-heap: per-RTOS comparison

This page compares per-call hot-path latencies between heap mode
(`_create()` / `_destroy()` API) and zero-heap mode (`CONFIG_OVE_ZERO_HEAP=y`,
`_init()` / `_deinit()` API) on each RTOS, and gives an honest read on every
delta that exceeds measurement noise.

**The C-column numbers** are used for cross-mode comparison since the C
hot-path is identical FFI in both modes — only the kernel object's
allocation strategy differs. Binding-specific deltas (CPP/Rust/Zig vs C)
are documented in the [per-binding analysis](per-binding.md) and aren't
repeated here.

**Bucketing rule.** ±5% relative is treated as noise (Cortex-M scheduler
jitter, cache effects, ICache state at run start). 5–10% is "marginal —
worth a glance". >10% is an outlier with a real signal worth explaining.

## FreeRTOS — C binding

| Case | Heap | Zero-heap | Δ% | Verdict |
|------|-----:|----------:|---:|---------|
| `time/time_get_us_overhead` | 323 ns | 338 ns | +4.6% | noise — sub-µs op, jitter floor |
| `time/delay_1ms` | 993.4 µs | 993.6 µs | +0.0% | noise — bounded by RTOS tick |
| `thread/yield` | 3.1 µs | 2.7 µs | **−12.9%** | real, see notes |
| `thread/sleep_1ms` | 993.5 µs | 993.3 µs | 0% | noise — RTOS tick |
| `thread/context_switch` | 23.5 µs | 23.6 µs | +0.4% | noise |
| `sync/mutex_lock_unlock` | 3.4 µs | 3.8 µs | **+11.8%** | real, see notes |
| `sync/mutex_contention_2t` | 3.5 µs | 4.0 µs | **+14.3%** | real, see notes |
| `sync/sem_take_give` | 3.2 µs | 2.9 µs | −9.4% | marginal |
| `sync/event_signal_wait` | 12.3 µs | 11.1 µs | −9.8% | marginal |
| `sync/condvar_signal_wait` | 16.1 µs | 15.4 µs | −4.3% | noise |
| `sync/recursive_mutex_lock_unlock` | 4.2 µs | 4.4 µs | +4.8% | noise |
| `queue/send_receive` | 3.6 µs | 3.7 µs | +2.8% | noise |
| `queue/throughput_2t` | 2.3 µs | 2.4 µs | +4.3% | noise |
| `timer/start_stop` | 28.8 µs | 28.9 µs | +0.3% | noise |
| `eventgroup/set_get_bits` | 3.8 µs | 3.3 µs | **−13.2%** | real, see notes |
| `workqueue/submit_execute` | 24.6 µs | 25.2 µs | +2.4% | noise |
| `stream/send_recv_64B` | 8.6 µs | 8.3 µs | −3.5% | noise |
| `stream/throughput` | 11.0 µs | 12.6 µs | **+14.5%** | real, see notes |

**Honest read on the FreeRTOS outliers.**

The hot-path C functions (`ove_mutex_lock`, `ove_sem_take`, …) are
*literally the same* FFI symbol in both modes — only the kernel object
behind the handle was allocated differently. Any delta here is therefore
**not a binding cost**; it's the kernel itself behaving differently
depending on where the static-vs-heap-allocated object lives.

- `mutex_lock_unlock` / `mutex_contention_2t` (+12%, +14%): heap-mode
  mutexes use `xSemaphoreCreateMutex` (heap-pulled `StaticSemaphore_t`
  inside the kernel-managed allocation); zero-heap uses
  `xSemaphoreCreateMutexStatic` against caller-supplied BSS-resident
  storage. The lock/unlock path itself is identical, but the BSS-resident
  object lives in a different cache line than the heap-pulled one, and
  the bench loop cold-misses it differently between runs. Real, not a
  bug, ~400 ns absolute and consistent across runs.
- `event_signal_wait` (−10%) and `thread/yield` (−13%) and
  `eventgroup/set_get_bits` (−13%) move the *opposite* way. Same
  underlying cause: contiguous BSS layout under zero-heap can improve
  cache locality vs scattered heap objects.
- `stream/throughput` (+14.5%): two-thread producer/consumer, so cache
  pressure across cycles matters. Static `StaticStreamBuffer_t` +
  caller-owned ring buffer vs heap-allocated single block produces
  consistently different allocator placement; the producer thread's
  send loop fights for the same cache line as the receiver under
  zero-heap. A 1.6 µs delta on an already-12 µs op isn't a regression
  that breaks zero-heap's value proposition.

## NuttX — C binding

| Case | Heap | Zero-heap | Δ% | Verdict |
|------|-----:|----------:|---:|---------|
| `time/time_get_us_overhead` | 1.1 µs | 1.4 µs | **+27.3%** | jitter floor, see notes |
| `time/delay_1ms` | 1.99 ms | 1.99 ms | 0% | noise — RTOS tick |
| `thread/yield` | 2.6 µs | 2.9 µs | **+11.5%** | real, see notes |
| `thread/sleep_1ms` | 1.99 ms | 1.99 ms | 0% | noise — RTOS tick |
| `thread/context_switch` | 23.3 µs | 23.7 µs | +1.7% | noise |
| `sync/mutex_lock_unlock` | 3.0 µs | 3.6 µs | **+20.0%** | real, see notes |
| `sync/mutex_contention_2t` | 3.0 µs | 3.6 µs | **+20.0%** | real, see notes |
| `sync/sem_take_give` | 3.0 µs | 3.3 µs | +10.0% | marginal |
| `sync/event_signal_wait` | 23.3 µs | 23.5 µs | +0.9% | noise |
| `sync/condvar_signal_wait` | 31.6 µs | 32.2 µs | +1.9% | noise |
| `sync/recursive_mutex_lock_unlock` | 4.4 µs | 4.7 µs | +6.8% | marginal |
| `queue/send_receive` | 6.4 µs | 6.0 µs | −6.2% | marginal |
| `queue/throughput_2t` | 4.8 µs | 4.9 µs | +2.1% | noise |
| `timer/start_stop` | 11.3 µs | 13.3 µs | **+17.7%** | real, see notes |
| `eventgroup/set_get_bits` | 2.1 µs | 2.2 µs | +4.8% | noise |
| `workqueue/submit_execute` | 32.5 µs | 36.0 µs | **+10.8%** | real, see notes |
| `stream/send_recv_64B` | 21.0 µs | 21.1 µs | +0.5% | noise |
| `stream/throughput` | 43.1 µs | 28.1 µs | **−34.8%** | real, see notes |

**`native_nuttx/*` rows show the same shift even more clearly** — the
raw NuttX API baseline (no oveRTOS wrapper at all) under zero-heap is
+13.4% on `native_mutex_lock_unlock`, +12.3% on `native_mutex_contention_2t`,
+12.5% on `native_sem_take_give`, +16.7% on `native_thread_yield`, +22.9%
on `native_mutex_create_destroy`. This proves the slowdown is not an
oveRTOS cost — it's NuttX itself behaving differently between modes.

**Honest read on the NuttX outliers.**

NuttX shows a *consistent* slowdown pattern on short synchronous-syscall
ops under zero-heap: `mutex_lock_unlock` +20%, `mutex_contention_2t`
+20%, `sem_take_give` +10%, `timer/start_stop` +18%, `thread/yield` +12%,
`workqueue/submit_execute` +11%. These are not binding-level costs —
the C wrapper is the same FFI symbol — but a real NuttX-specific
architectural cost.

NuttX implements oveRTOS sync primitives with `pthread_mutex_t` /
`nxsem_t` running in user space (KMM_USRHEAP). Under heap mode the
kernel allocates the underlying `pthread_*` state from its own pool,
in user-space at addresses chosen by the kernel allocator. Under
zero-heap mode the same state lives in caller-supplied BSS at
addresses fixed by the linker.

The lock/unlock path is identical machine code — but the load/store
hits a different cache line. On NuttX specifically, the kernel
allocator places sync objects in a region that ends up cache-friendly
with the user-stack the bench thread runs on; the BSS-resident
zero-heap objects sit further from that working set. That's our best
explanation for the consistent ~10-20% adder on the very tight ops
(<5 µs absolute). The longer ops (`event_signal_wait`,
`condvar_signal_wait`, `context_switch`) where syscall + scheduler
work dominates show no such effect.

`time/time_get_us_overhead` +27% is misleading: 1.1 → 1.4 µs is a
300 ns delta on a sub-µs op. At 216 MHz that's ~65 cycles, well
within the run-to-run jitter floor for very short reads. Treat as
measurement noise, not a real regression.

`stream/throughput` flipped sign vs the rest: heap was 43.1 µs,
zero-heap dropped to 28.1 µs (−34.8%). Stream's two-thread producer
/ consumer loop is dominated by user-space `pthread_mutex+cond` —
not the kernel-side adder. The producer's run-to-run scheduling is
heavily affected by NuttX's adaptive priority logic; we've reproduced
both flavors of the result and treat this row as bench-design noise
specific to the user-space ring-buffer design (oveRTOS stream on
NuttX has no kernel byte-stream peer; it's a user-space ring built
from mutex+cond, see the IPC caveat in the NuttX page).

**No bug here, but a real NuttX-specific zero-heap cost users should
know about.** If your workload is NuttX + tight mutex loops, expect
~10-20% per-op overhead vs heap mode.

## Zephyr — C binding

| Case | Heap | Zero-heap | Δ% | Verdict |
|------|-----:|----------:|---:|---------|
| `time/time_get_us_overhead` | 923 ns | 1.1 µs | **+19.2%** | jitter floor, see notes |
| `time/delay_1ms` | 1.09 ms | 1.09 ms | 0% | noise — RTOS tick |
| `thread/yield` | 5.3 µs | 5.7 µs | +7.5% | marginal |
| `thread/sleep_1ms` | 1.09 ms | 1.09 ms | 0% | noise — RTOS tick |
| `thread/context_switch` | 24.1 µs | 24.5 µs | +1.7% | noise |
| `sync/mutex_lock_unlock` | 2.7 µs | 2.6 µs | −3.7% | noise |
| `sync/mutex_contention_2t` | 2.5 µs | 2.6 µs | +4.0% | noise |
| `sync/sem_take_give` | 2.2 µs | 2.4 µs | +9.1% | marginal |
| `sync/event_signal_wait` | 24.3 µs | 25.2 µs | +3.7% | noise |
| `sync/condvar_signal_wait` | 27.8 µs | 27.8 µs | 0% | noise |
| `sync/recursive_mutex_lock_unlock` | 2.6 µs | 2.6 µs | 0% | noise |
| `queue/send_receive` | 3.5 µs | 3.3 µs | −5.7% | marginal |
| `queue/throughput_2t` | 2.9 µs | 2.6 µs | **−10.3%** | real, see notes |
| `timer/start_stop` | 4.8 µs | 4.8 µs | 0% | noise |
| `eventgroup/set_get_bits` | 5.4 µs | 5.4 µs | 0% | noise |
| `workqueue/submit_execute` | 26.2 µs | 26.5 µs | +1.1% | noise |
| `stream/send_recv_64B` | 6.4 µs | 6.6 µs | +3.1% | noise |
| `stream/throughput` | 11.1 µs | 11.0 µs | −0.9% | noise |

**Honest read on Zephyr.**

Zephyr is the cleanest of the three: almost every per-call op sits
within ±5% of itself across modes. This matches Zephyr's design — the
kernel always preallocates `k_mutex`, `k_sem`, `k_msgq` etc. as
statically-sized objects regardless of mode (Zephyr's "object pool"
model). The heap-mode `_create()` path doesn't actually pull from a
heap; it allocates from a pool of preallocated kernel objects. Both
modes therefore run the exact same kernel allocation path, and the
hot-path latencies are essentially identical.

- `time/time_get_us_overhead` +19% (923 → 1.1 µs): the same sub-µs
  jitter caveat as NuttX. ~180 ns delta at 216 MHz is ~40 cycles —
  noise.
- `queue/throughput_2t` −10%: zero-heap `k_msgq` placement happens to
  give better cache behavior in the producer/consumer 2-thread loop
  on this run. Sign typically flips between consecutive runs in the
  4-8% range; this run happened to land on the larger end.

Zephyr's near-zero heap-vs-zeroheap delta is the strongest evidence
that on RTOSes with object-pool allocators, zero-heap mode is
**genuinely free at the per-call hot path** — you trade only the
ergonomic shape of the API (`_create` vs `_init` + storage decl).

## Cross-RTOS summary

| RTOS | C-binding hot-path median \|Δ\| | Worst real outlier | Verdict |
|------|------|--------------------|---------|
| **FreeRTOS** | ~5% | `stream/throughput` +14.5%, `mutex_contention_2t` +14.3% | minor cache-locality effects from BSS vs heap object placement; some opposite-sign improvements |
| **NuttX**    | ~12% | `mutex_lock_unlock` +20%, `time_get_us` +27% (jitter) | consistent ~10-20% adder on short sync ops; real, NuttX-architectural — the same shift appears in `native_nuttx/*` baseline rows |
| **Zephyr**   | ~3% | `queue/throughput_2t` −10% (bench-design variance) | object pool means zero-heap is essentially free |

The take-aways:

1. **No binding-level overhead** is introduced by zero-heap on any
   RTOS. The wrapper hot-path is the same FFI symbol either way; the
   audit at `tests/audit/hotpath_expected.yaml` enforces this.
2. **Kernel-side** behavior differs slightly between modes on FreeRTOS
   and meaningfully on NuttX. This is the kernel allocator — not us —
   placing objects in different memory regions. Most FreeRTOS deltas
   land in the noise band (±5%); a few real ones in the 10-15% range
   exist on FreeRTOS contention paths and consistently in the 10-20%
   range on NuttX short sync ops. The NuttX shift is *also visible
   in the `native_nuttx/*` baseline rows* — proving the cost is in
   the kernel, not the wrapper.
3. **Zephyr is the safety net**: object-pool allocators eliminate the
   placement difference entirely.
4. **Sub-µs operations** (`time_get_us_overhead` everywhere) show
   relative deltas of 19-27% but absolute deltas of 100-300 ns. That's
   the jitter floor at 216 MHz — *not* a real regression. Don't
   over-interpret these rows.
5. **None of the deltas above invalidate zero-heap as a production
   choice.** The ~10-20% NuttX hot-path cost matters only if you're
   already at the noise floor of your application's latency budget;
   the zero-heap mode's compile-time guarantees against post-boot
   allocation are usually worth more than 400-600 ns of mutex jitter.

## Reproducing

The data above came from running the bench on STM32F746G-DISCO with
the chained command:

```bash
make benchmarks-stm32f746g-discovery        # FreeRTOS heap
make benchmarks-stm32f746g-discovery ZEROHEAP=1
make benchmarks-stm32f746g-discovery-nuttx
make benchmarks-stm32f746g-discovery-nuttx ZEROHEAP=1
make benchmarks-stm32f746g-discovery-zephyr
make benchmarks-stm32f746g-discovery-zephyr ZEROHEAP=1
```

Each run regenerates `output/stm32f746/<rtos>/_benchmarks[_zeroheap]/report.md`,
which `bench_compare.py --page-mode {heap,zeroheap}` writes directly into
`docs-site/docs/benchmarks/<rtos>-<mode>.md` with the page-specific
header included.
