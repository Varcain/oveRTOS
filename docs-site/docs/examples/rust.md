# Rust Example

Source: `apps/rust/example/src/lib.rs`

The Rust example demonstrates the oveRTOS Rust SDK. It implements the same producer-consumer pattern as the C and C++ examples with idiomatic Rust error handling, `no_std` support, and safe thread entry functions.

## Crate attributes and imports

```rust
#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicU32, Ordering};
use ove::{Error, FmtBuf, Priority, Queue, Thread, Timer, WAIT_FOREVER};
#[cfg(has_lvgl)]
use ove::lvgl::{self, Bar, Color, Label, Layout, Styleable};
```

The crate supports both `std` and `no_std` targets. On embedded boards the `std` feature is disabled and the crate runs entirely without the Rust standard library. The `has_lvgl` cfg flag is set by the build system when LVGL is available.

## Shared state with atomics and the `shared!` macro

```rust
ove::shared!(QUEUE: Queue<u32, 8>);
static LAST_VALUE: AtomicU32 = AtomicU32::new(0);
#[cfg(has_lvgl)]
ove::shared!(UI_TIMER: Timer);
```

`ove::shared!` declares a lazily-initialized `static` that can be shared safely between threads without a mutex. The `QUEUE` and `UI_TIMER` are initialized in `app_main` before the threads that use them are spawned. `LAST_VALUE` uses a lock-free atomic instead of a mutex, matching the simple single-writer/single-reader pattern.

## Thread entry functions

Thread entries are plain Rust functions with no `unsafe` or `extern "C"`:

```rust
fn producer_entry() {
    ove::log_inf!("Producer started");
    let mut count: u32 = 0;

    loop {
        count += 1;
        match QUEUE.send(&count, 1000) {
            Ok(()) => {}
            Err(Error::Timeout) => {
                ove::log_wrn!("Producer: send timeout");
            }
            Err(Error::QueueFull) => {
                ove::log_wrn!("Producer: queue full, dropped {}", count);
            }
            Err(_) => {
                ove::log_err!("Producer: unexpected send error");
            }
        }
        Thread::sleep_ms(500);
    }
}
```

Error handling uses Rust's `match` on `Result<(), ove::Error>`. The `Error` enum variants map directly onto the oveRTOS C error codes and are used with the structured logging macros. `ove::log_inf!`, `ove::log_wrn!`, and `ove::log_err!` automatically add the `[I]`/`[W]`/`[E]` prefix and newline, and format into a stack-allocated buffer without heap allocation, compatible with `no_std`.

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

The consumer uses `Ordering::Relaxed` for the atomic store because `LAST_VALUE` is only ever read from the UI timer callback which runs at a known cadence — no synchronization ordering is needed.

## LVGL UI

```rust
#[cfg(has_lvgl)]
fn ui_timer_cb() {
    let val = LAST_VALUE.load(Ordering::Relaxed);
    let _g = lvgl::lock();   // guard auto-releases on drop

    if let Some(label) = COUNTER_LABEL.try_get() {
        let mut buf = [0u8; 24];
        let mut w = FmtBuf::new(&mut buf);
        let _ = write!(w, "Count: {}", val);
        label.set_text(w.as_cstr());
    }
    if let Some(bar) = BAR.try_get() {
        bar.set_value((val % 101) as i32, true);
    }
}
```

`lvgl::lock()` returns a guard that releases the LVGL lock when dropped. `FmtBuf` is a no-alloc `Write` implementation that formats into a fixed stack buffer and produces a null-terminated C string pointer via `as_cstr()`, suitable for LVGL's `lv_label_set_text`.

`COUNTER_LABEL.try_get()` returns `Option<&Label>` — it returns `None` if the widget has been shut down, avoiding use-after-free at shutdown.

## App entry point

```rust
fn app_main() {
    ove::log_inf!("Rust example: init");

    QUEUE.init(ove::queue!(u32, 8));

    #[cfg(has_lvgl)]
    let _graphics = ove::thread!("graphics", graphics_entry, Priority::High, 4096);
    let _producer = ove::thread!("producer", producer_entry, Priority::Normal, 4096);
    let _consumer = ove::thread!("consumer", consumer_entry, Priority::Normal, 4096);

    #[cfg(has_lvgl)]
    {
        UI_TIMER.init(ove::timer!(ui_timer_cb, 200, false));
        lvgl::init().expect("LVGL init failed");
        { let _g = lvgl::lock(); create_ui(); }
        UI_TIMER.start().expect("Timer start failed");
    }

    ove::log_inf!("Rust example: ready");
    ove::run();

    // Cleanup (reached only on POSIX)
    QUEUE.shutdown();
}

ove::main!(app_main);
```

`ove::thread!` is a macro that calls the C `ove_thread_create` API and wraps the Rust function in a trampoline. `ove::main!(app_main)` generates the `ove_main` C symbol that the oveRTOS platform entry point calls. `ove::run()` starts the scheduler.

## Key APIs demonstrated

| API | Purpose |
|-----|---------|
| `ove::shared!` | Lazily-initialized static, thread-safe |
| `Queue<T, N>` | Type-safe fixed-depth queue |
| `Queue::send` / `Queue::receive` | Blocking inter-thread data transfer |
| `AtomicU32` | Lock-free shared counter |
| `ove::thread!` | Spawn a thread from a safe Rust fn |
| `ove::timer!` | Create a periodic timer |
| `ove::log_inf!` / `ove::log_wrn!` / `ove::log_err!` | Structured console logging with auto prefix |
| `FmtBuf` | Stack-allocated format buffer |
| `lvgl::lock()` | RAII LVGL display guard |
| `ove::run()` | Start the RTOS scheduler |
| `ove::main!` | Export `ove_main` entry point |

---

## Networking Example

Source: `apps/rust/example_net/src/lib.rs`

The Rust networking example demonstrates the safe Rust networking API with a structured test harness (TEST/PASS/FAIL tracking with results summary). DNS, TCP, UDP, HTTP (GET/POST/PUT), SNTP time sync, MQTT pub/sub, and HTTPD are all tested using `ove::net`, `ove::net_http`, `ove::net_sntp`, `ove::net_mqtt`, and `ove::net_httpd` modules. All logging uses structured macros (`ove::log_inf!`, etc.). See the [Networking API Guide](../api/net.md) for the full API.
