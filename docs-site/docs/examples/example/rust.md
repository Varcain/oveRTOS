# Basic Example — Rust

Source: `apps/rust/example/src/lib.rs` | **[WASM Demo](https://varcain.github.io/oveRTOS/example_rust/){:target="_blank"}**

The Rust example demonstrates the `ove` crate with `no_std` support, atomic shared state, `Result`-based error handling, and compile-time RTOS detection.

## Rust SDK patterns

Objects use the `ove::shared!` macro for safe static initialization:

```rust
#![cfg_attr(not(feature = "std"), no_std)]

use ove::{Queue, Thread, Timer, Priority, WAIT_FOREVER};
use core::sync::atomic::{AtomicU32, Ordering};

ove::shared!(QUEUE: Queue<u32, 8>);
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);
```

`ove::shared!` creates a `OnceCell`-style wrapper that initializes the oveRTOS object on first access. `AtomicU32` replaces the mutex for the counter — lock-free shared state.

## Compile-time RTOS detection

```rust
#[cfg(rtos_freertos)]
const APP_TITLE: &[u8] = b"oveRTOS(FreeRTOS) Rust Demo\0";
#[cfg(rtos_posix)]
const APP_TITLE: &[u8] = b"oveRTOS(POSIX) Rust Demo\0";
```

The `build.rs` emits `rtos_freertos`, `rtos_posix`, etc. as `cfg` flags based on `ove_config.h`.

## Producer thread

```rust
fn producer(_: *mut core::ffi::c_void) {
    let mut count: u32 = 0;
    ove::log_inf!("Producer started");

    loop {
        count += 1;
        if let Err(_) = QUEUE.get().send(&count, 1000) {
            ove::log_wrn!("Producer: queue full, dropped {}", count);
        }
        Thread::sleep_ms(500);
    }
}
```

`QUEUE.get()` returns `&Queue<u32, 8>`. Error handling uses `Result<(), Error>`.

## Consumer with atomic state

```rust
fn consumer(_: *mut core::ffi::c_void) {
    loop {
        let mut val: u32 = 0;
        if QUEUE.get().receive(&mut val, WAIT_FOREVER).is_ok() {
            LAST_VALUE.store(val, Ordering::Relaxed);
            if val % 5 == 0 {
                ove::log_inf!("Consumer: count = {}", val);
            }
        }
    }
}
```

No mutex needed — `AtomicU32` provides lock-free access to the shared counter.

## Entry point

```rust
#[no_mangle]
pub extern "C" fn ove_main() {
    ove::log_inf!("Rust example: init");

    QUEUE.init(Queue::new()?);

    Thread::spawn(producer, Priority::Normal, "producer", 4096)?;
    Thread::spawn(consumer, Priority::Normal, "consumer", 4096)?;

    #[cfg(has_lvgl)]
    {
        ove::lvgl::init()?;
        create_ui();
        UI_TIMER.init(Timer::periodic(ui_timer_cb, 200)?);
        UI_TIMER.get().start()?;
    }

    ove::run();
}
```

## Key APIs demonstrated

| Rust API | C Equivalent | Purpose |
|----------|-------------|---------|
| `ove::Queue<T, N>::new()` | `ove_queue_create` | Typed fixed-size queue |
| `ove::shared!` | Static initialization | Safe `OnceCell`-style globals |
| `AtomicU32` | `ove_mutex_*` | Lock-free shared state |
| `Thread::spawn()` | `ove_thread_create` | Thread creation with Result |
| `Timer::periodic()` | `ove_timer_create` | Periodic callback timer |
| `ove::lvgl::init()` | `ove_lvgl_init` | LVGL initialization |
| `ove::run()` | `ove_run` | Start scheduler |

## How to build

```bash
make host.posix.example_rust    # or wasm.posix.example_rust
make configure && make download && make && make run
```
