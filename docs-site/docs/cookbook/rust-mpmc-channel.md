# MPMC channel between Rust threads (`ove::channel`)

**Pattern** — multiple producer threads, one or more consumer threads,
exchanging messages through a fixed-capacity FIFO. Crossbeam-style
`Sender` / `Receiver` halves are both cloneable; dropping all of one
side closes the channel to the other.

Rust-only. The underlying [`ove_queue`](../api/ipc.md#queue) is
language-agnostic — if you want a C / C++ / Zig producer or consumer
on the same queue, use `ove::Queue::from_static` and the bare FFI.

## What to enable

`app.yaml`:

```yaml
defconfig:
  - CONFIG_OVE_QUEUE=y
  - CONFIG_OVE_SYNC=y         # any non-trivial thread needs sync prims
  - CONFIG_OVE_TIMER=y        # for sleep / timeout demos
```

`Cargo.toml`: nothing extra. `ove::channel` is built whenever
`CONFIG_OVE_QUEUE=y`. Heap mode uses an internal `Arc`; enable
`features = ["alloc"]` (already implied by `std`).

## Heap mode

```rust
use ove::channel;
use ove::{Priority, Thread};

#[ove::main]
fn app_main() {
    ove::log::try_init();

    let (tx, rx) = channel::channel::<u32, 8>()
        .expect("channel create");

    // Producer: clone the Sender into the closure. The channel-side
    // refcount keeps the queue alive for the program lifetime.
    let tx_p = tx.clone();
    let producer = Thread::builder()
        .name(c"producer")
        .priority(Priority::Normal)
        .stack_size(4096)
        .spawn(move |_tok| {
            for n in 0u32.. {
                match tx_p.try_send(n) {
                    Ok(()) => {}
                    Err(ove::Error::QueueFull) => {
                        log::warn!("queue full, dropped {n}")
                    }
                    Err(ove::Error::NetClosed) => break,  // all receivers gone
                    Err(e) => log::error!("send failed: {e:?}"),
                }
                Thread::sleep_ms(500);
            }
        })
        .expect("producer spawn");

    // Consumer: move the Receiver in.
    let consumer = Thread::builder()
        .name(c"consumer")
        .priority(Priority::Normal)
        .stack_size(4096)
        .spawn(move |_tok| {
            loop {
                match rx.recv() {
                    Ok(n) => log::info!("got {n}"),
                    Err(ove::Error::NetClosed) => break,  // all senders gone
                    Err(e) => log::error!("recv failed: {e:?}"),
                }
            }
        })
        .expect("consumer spawn");

    // Threads outlive the closure — detach the handles so Drop doesn't
    // call request_stop when app_main exits.
    core::mem::forget(producer);
    core::mem::forget(consumer);

    ove::run();
}
```

## Zero-heap mode

The static path drops the internal `Arc` and uses caller-owned counters
plus a static `Queue`:

```rust
use core::sync::atomic::AtomicUsize;
use ove::channel::{Sender, Receiver};
use ove::Queue;

static QUEUE: Queue<u32, 8> = ove::queue!(static u32, 8);
static TX_COUNT: AtomicUsize = AtomicUsize::new(1);  // initial sender
static RX_COUNT: AtomicUsize = AtomicUsize::new(1);  // initial receiver

#[ove::main]
fn app_main() {
    // SAFETY: QUEUE / TX_COUNT / RX_COUNT are file-scope statics
    // owned exclusively by this channel pair.
    let tx = unsafe { Sender::<u32, 8>::from_static(&QUEUE, &TX_COUNT, &RX_COUNT) };
    let rx = unsafe { Receiver::<u32, 8>::from_static(&QUEUE, &TX_COUNT, &RX_COUNT) };
    // ... same producer/consumer spawn pattern as heap mode ...
}
```

In zero-heap, the channel halves still clone correctly — the
`AtomicUsize` counts track all live `Sender` / `Receiver` instances
including the ones cloned into spawned threads.

## What you get over `Queue<T, N>` directly

- **`Sender` / `Receiver` are `Clone`** — no `Arc::clone(&queue)`
  juggling, no `Arc<Queue<u32, 8>>` typing noise.
- **Half-closed detection** — `recv()` returns
  [`Error::NetClosed`](../api/index.md#error-codes) once every
  `Sender` has been dropped *and* the queue is drained. `send()`
  returns the same once every `Receiver` is gone. Bare `Queue::recv`
  can't tell because it has no notion of producer lifetime.
- **Single API across heap and zero-heap** — the `channel()` factory
  for heap, `Sender::from_static` / `Receiver::from_static` for
  zero-heap; everything downstream is identical.

## See also

- [`ove::channel` rustdoc](../api/rust/ove/channel/)
- [`ove_queue` C API](../api/ipc.md#queue) — what the channel sits on top of
- [`embassy_sync::Channel`](https://docs.embassy.dev/embassy-sync/git/default/channel/struct.Channel.html)
  — async equivalent for tasks on the embassy executor (different
  pattern: pure-Rust state, no `ove_queue`).
