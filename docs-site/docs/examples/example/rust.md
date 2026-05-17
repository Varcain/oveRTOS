# Basic Example — Rust

Source: `apps/rust/example/src/lib.rs` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_rust_heap/){:target="_blank"}**

The Rust example demonstrates the `ove` crate with `no_std` support, atomic shared state, `Result`-based error handling, and compile-time RTOS detection.

## Rust SDK patterns

Objects use the `ove::shared!` macro for safe static initialization:

```rust
#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicU32, Ordering};
use ove::{Error, Priority, Queue, Thread, Timer, WAIT_FOREVER};

ove::shared!(QUEUE: Queue<u32, 8>);
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);
```

`ove::shared!` expands to `static NAME: StaticCell<T>` — a `OnceCell`-style wrapper. Once initialized, `Deref` lets you call inner methods directly (e.g. `QUEUE.send(...)` rather than `QUEUE.get().send(...)`). `AtomicU32` replaces the mutex for the counter — lock-free shared state.

## Compile-time RTOS detection

```rust
#[cfg(rtos_freertos)]
const APP_TITLE: &[u8] = b"oveRTOS(FreeRTOS) Rust Demo\0";
#[cfg(rtos_posix)]
const APP_TITLE: &[u8] = b"oveRTOS(POSIX) Rust Demo\0";
```

The `build.rs` emits `rtos_freertos`, `rtos_posix`, etc. as `cfg` flags based on `ove_config.h`.

## Producer thread

Thread entries are plain safe `fn()` — no `unsafe extern "C"` ceremony, no raw pointer argument. The `ove::thread!` macro handles the C-ABI shim:

```rust
fn producer_entry() {
    let mut count: u32 = 0;
    ove::log_inf!("Producer started");

    loop {
        count += 1;
        match QUEUE.send(&count, core::time::Duration::from_millis(1000)) {
            Ok(())                  => {}
            Err(Error::QueueFull)   => ove::log_wrn!("Producer: queue full, dropped {}", count),
            Err(_)                  => ove::log_err!("Producer: send error"),
        }
        Thread::sleep_ms(500);
    }
}
```

`Queue::send` returns `Result<(), Error>`. Error variants are matched by name — no integer codes leak into app code.

## Consumer with atomic state

```rust
fn consumer_entry() {
    loop {
        match QUEUE.receive(WAIT_FOREVER) {
            Ok(val) => {
                LAST_VALUE.store(val, Ordering::Relaxed);
                if val % 5 == 0 {
                    ove::log_inf!("Consumer: count = {}", val);
                }
            }
            Err(_) => ove::log_err!("Consumer: receive error"),
        }
    }
}
```

`Queue::receive` returns `Result<T, Error>` with the value by value — no out-pointer. No mutex needed; `AtomicU32` provides lock-free access to the shared counter.

## Entry point

```rust
fn app_main() {
    ove::log_inf!("Rust example: init");

    QUEUE.init(ove::queue!(u32, 8));

    let _producer = ove::thread!("producer", producer_entry, Priority::Normal, 4096);
    let _consumer = ove::thread!("consumer", consumer_entry, Priority::Normal, 4096);

    UI_TIMER.init(ove::timer!(ui_timer_cb, 200, false));
    if lvgl::init().is_err() { return; }
    {
        let _g = lvgl::lock();
        create_ui();
    }
    let _ = UI_TIMER.start();

    ove::run();
}

ove::main!(app_main);
```

`ove::main!(app_main)` expands to the `#[unsafe(no_mangle)] pub extern "C" fn ove_main()` symbol the platform `main()` calls into — the user-facing function stays plain Rust. `ove::queue!`, `ove::thread!`, `ove::timer!` are the unified creation macros; they work in both heap and zero-heap modes.

## Key APIs demonstrated

| Rust API | C Equivalent | Purpose |
|----------|-------------|---------|
| `ove::shared!` | Static initialization | Safe `OnceCell`-style globals |
| `ove::queue!(T, N)` | `ove_queue_create` | Typed fixed-size queue |
| `ove::thread!(...)` | `ove_thread_create` | Safe thread spawn (no raw pointer arg) |
| `ove::timer!(...)` | `ove_timer_create` | Periodic callback timer |
| `ove::main!(fn)` | `void ove_main(void)` | Export the C-ABI entry point |
| `AtomicU32` | `ove_mutex_*` | Lock-free shared state |
| `Result<T, ove::Error>` | `int rc` | Typed errors instead of int codes |
| `ove::lvgl::init()` | `ove_lvgl_init` | LVGL initialization |
| `ove::run()` | `ove_run` | Start scheduler |

## How to build

```bash
make host.posix.example_rust    # or wasm.posix.example_rust
make configure && make download && make && make run
```
