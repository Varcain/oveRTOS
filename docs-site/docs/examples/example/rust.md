# Basic Example — Rust

Source: `apps/rust/heap/example/src/lib.rs` (also `apps/rust/zeroheap/example/src/lib.rs`) | **[WASM Demo](https://varcain.github.io/oveRTOS/example_rust_heap/){:target="_blank"}**

The Rust example demonstrates the `ove` crate with `no_std` support, `Arc`-shared kernel objects across threads, atomic counters, narrow `Result`-based error handling, `core::time::Duration`-typed timeouts, and compile-time RTOS detection. Heap mode and zero-heap mode share the same wrapper types — only the construction step differs.

## Heap mode — `Arc`, `Thread::builder`, narrow errors

In heap mode each kernel object is allocated via `Type::new()` and wrapped in `Arc<T>` for sharing across threads. Threads spawn via `Thread::builder()` and take a `FnOnce` closure that captures `Arc::clone`s by `move`. `#[ove::main]` is the proc-macro that emits the C-ABI `ove_main` entry symbol — the user-facing function stays plain Rust.

```rust
#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;
use ove::heap::Arc;
use ove::{InitCell, Priority, Queue, Thread, Timer};

// Pull in the libc-malloc-backed `#[global_allocator]` for bare-metal
// builds. On host (`feature = "std"`) it's a no-op.
use ove_allocator as _;

static UI_TIMER: InitCell<Timer> = InitCell::new();
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);

#[ove::main]
fn app_main() {
    ove::log::try_init();
    log::info!("Rust example (heap mode): init");

    // Heap-allocate the queue; Arc::clone hands each thread its own
    // refcount. The kernel handle inside Queue<u32, 8> drops when the
    // last Arc does.
    let queue: Arc<Queue<u32, 8>> = Arc::new(Queue::new().expect("queue"));
    let last_value: Arc<AtomicU32> = Arc::new(AtomicU32::new(0));

    let q = Arc::clone(&queue);
    let _producer = Thread::builder()
        .name(c"producer")
        .priority(Priority::Normal)
        .stack_size(4096)
        .spawn(move |_stop| {
            let mut count: u32 = 0;
            loop {
                count += 1;
                // try_send_for narrow error set: Timeout | QueueFull only.
                match q.try_send_for(&count, Duration::from_millis(1000)) {
                    Ok(()) => {}
                    Err(ove::Error::Timeout) => log::warn!("send timeout"),
                    Err(ove::Error::QueueFull) => {
                        log::warn!("queue full, dropped {count}")
                    }
                    Err(_) => unreachable!(), // narrow set
                }
                Thread::sleep_ms(500);
            }
        })
        .expect("producer spawn");

    let q = Arc::clone(&queue);
    let lv = Arc::clone(&last_value);
    let _consumer = Thread::builder()
        .name(c"consumer")
        .priority(Priority::Normal)
        .stack_size(4096)
        .spawn(move |_stop| loop {
            // Forever recv() is infallible — returns T directly.
            if let Ok(v) = q.recv() {
                lv.store(v, Ordering::Relaxed);
                LAST_VALUE.store(v, Ordering::Relaxed);
                if v % 5 == 0 {
                    log::info!("Consumer: count = {v}");
                }
            }
        })
        .expect("consumer spawn");

    UI_TIMER.init(Timer::new(ui_timer_cb, 200, false).expect("timer"));
    if lvgl::init().is_err() {
        return;
    }
    {
        let _g = lvgl::lock();
        create_ui();
    }
    let _ = UI_TIMER.start();

    ove::run();
}
```

### What changes between heap and zero-heap

Same wrapper types, same method calls. Construction differs:

| | Heap mode | Zero-heap mode |
|---|---|---|
| Queue | `Queue::<u32, 8>::new()` | `ove::queue!(u32, 8)` (expands to `Queue::from_static(&mut S, ...)`) |
| Mutex | `Mutex::new(state)` | `ove::mutex!(state)` |
| Thread | `Thread::builder().spawn(closure)` | same, plus optional `ove::thread!(name, fn, prio, stack)` for the bare-fn case |
| Sharing | `Arc<T>` clones | `ove::shared!(NAME: T)` → `InitCell<T>` static |
| Global allocator | `use ove_allocator as _;` (libc-malloc shim) | none — `alloc` not linked |

The zero-heap variant of this app lives at `apps/rust/zeroheap/example/src/lib.rs`. It uses `ove::shared!` cells instead of `Arc`, and `ove::queue!` / `ove::thread!` instead of `Type::new()`:

```rust
#![cfg_attr(not(feature = "std"), no_std)]
use ove::{Priority, Queue, Thread};

ove::shared!(QUEUE: Queue<u32, 8>);     // static QUEUE: InitCell<Queue<u32,8>>

fn producer_entry() {
    let mut count: u32 = 0;
    loop {
        count += 1;
        let _ = QUEUE.try_send_for(&count, core::time::Duration::from_millis(1000));
        Thread::sleep_ms(500);
    }
}

#[ove::main]
fn app_main() {
    ove::log::try_init();
    QUEUE.init(ove::queue!(u32, 8));    // → Queue::from_static(&mut S, ...)
    let _ = ove::thread!("producer", producer_entry, Priority::Normal, 4096);
    ove::run();
}
```

## Compile-time RTOS detection

`build.rs` emits `rtos_freertos`, `rtos_nuttx`, `rtos_zephyr`, `rtos_posix` as `cfg` flags based on the substrate's `ove_config.h`:

```rust
#[cfg(rtos_freertos)]
const APP_TITLE: &[u8] = b"oveRTOS(FreeRTOS) Rust Demo\0";
#[cfg(rtos_nuttx)]
const APP_TITLE: &[u8] = b"oveRTOS(NuttX) Rust Demo\0";
#[cfg(rtos_zephyr)]
const APP_TITLE: &[u8] = b"oveRTOS(Zephyr) Rust Demo\0";
#[cfg(rtos_posix)]
const APP_TITLE: &[u8] = b"oveRTOS(POSIX) Rust Demo\0";
```

## Logging

The standard `log` crate (`log::info!`, `log::warn!`, `log::error!`, `log::debug!`) routes through the oveRTOS console once `ove::log::try_init()` has been called. Any third-party crate using `log::` macros logs through the same path.

```rust
ove::log::try_init();
log::info!("Producer started, queue at {:p}", &*queue);
```

## Cooperative cancellation

The closure passed to `Thread::builder().spawn(...)` receives a `StopToken`. Outside code calls `handle.request_stop()` (returned from `spawn`); the worker observes `stop.is_stopped()` at its own polling points:

```rust
let handle = Thread::builder()
    .name(c"worker")
    .spawn(move |stop| {
        while !stop.is_stopped() {
            // do work…
            Thread::sleep_ms(10);
        }
        log::info!("worker exiting cleanly");
    })
    .expect("spawn");

// later, from anywhere:
handle.request_stop();
```

## Key APIs demonstrated

| Rust API | C Equivalent | Purpose |
|----------|-------------|---------|
| `Queue::<T, N>::new()` | `ove_queue_create` | Typed fixed-size queue (heap) |
| `ove::queue!(T, N)` | `ove_queue_init` + storage | Same, zero-heap-friendly macro |
| `Arc<Queue<...>>` | manual refcount | Shared kernel object across threads |
| `Thread::builder()` | `ove_thread_create` | Closure-capturing spawn with stop-token |
| `Timer::new(cb, ms, oneshot)` | `ove_timer_create` | Periodic callback timer |
| `InitCell<T>` | static initialization | OnceCell-style global cell |
| `#[ove::main]` | `void ove_main(void)` | Export the C-ABI entry point |
| `ove::log::try_init()` + `log::info!` | `OVE_LOG_INF` | std `log` crate integration |
| `AtomicU32` | `ove_mutex_*` over int | Lock-free shared counter |
| `Result<T, ove::Error>` | `int rc` | Typed errors; narrow per-op sets |
| `core::time::Duration` | `uint64_t timeout_ns` | Typed timeouts |
| `lvgl::init()` / `lvgl::lock()` | `ove_lvgl_init` | LVGL bring-up + cross-thread mutex |
| `ove::run()` | `ove_run` | Start scheduler |

## How to build

```bash
make host.posix.example_rust            # heap mode on POSIX
make host.posix.example_rust_zh         # zero-heap mode on POSIX
make configure && make download && make && make run
```
