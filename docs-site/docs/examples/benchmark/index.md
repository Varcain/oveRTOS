# Benchmark

The benchmark application measures latency, throughput, and memory usage of all oveRTOS abstractions. Results are printed as formatted ASCII tables via `OVE_LOG`.

## What it measures

| Suite | Metrics |
|-------|---------|
| **Thread** | Creation latency, context switch time, yield overhead |
| **Queue** | Send/receive latency, throughput (messages/sec), full-queue behavior |
| **Mutex** | Lock/unlock latency, contention overhead |
| **Timer** | Start latency, callback jitter, periodic accuracy |
| **Memory** | Heap usage (total, used, free), allocation throughput |
| **Stream** | Write/read throughput (bytes/sec), trigger latency |
| **Workqueue** | Schedule latency, deferred execution overhead |
| **Event Group** | Set/wait latency, multi-bit pattern matching |

Each suite runs configurable iterations (default 1000) and reports min/max/avg/median statistics.

## Language implementations

| Language | Source | WASM Demo |
|----------|--------|-----------|
| [C](c.md) | `tests/benchmarks/c/` | **[Run in browser](https://varcain.github.io/oveRTOS/benchmark/){:target="_blank"}** |
| [C++](cpp.md) | `tests/benchmarks/cpp/` | **[Run in browser](https://varcain.github.io/oveRTOS/benchmark_cpp/){:target="_blank"}** |
| [Rust](rust.md) | `tests/benchmarks/rust/` | **[Run in browser](https://varcain.github.io/oveRTOS/benchmark_rust/){:target="_blank"}** |
| [Zig](zig.md) | `tests/benchmarks/zig/` | *Not yet available* |

## How to build

```bash
make host.posix.benchmark     # C
make host.posix.benchmark_cpp # C++
make configure && make download && make && make run
```

## Sample output

```
=== oveRTOS Benchmark ===
  Thread creation:  min=12us  avg=18us  max=31us  (1000 iterations)
  Queue send/recv:  min=2us   avg=3us   max=8us   (1000 iterations)
  Mutex lock/unlock: min=1us  avg=1us   max=4us   (1000 iterations)
  ...
```
