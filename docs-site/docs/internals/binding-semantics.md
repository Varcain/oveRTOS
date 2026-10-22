# Binding Semantics

!!! note "Audience"
    This page is for **oveRTOS developers** — people writing or reviewing the
    C++, Rust, and Zig bindings (`bindings/{cpp,rust,zig}/ove/`). It is the
    single source of truth for *what each wrapper must do*. App developers
    writing against one language can read the [API Reference](../api/index.md)
    instead; this page exists so the three bindings stay behaviourally
    identical.

The three bindings deliberately look different — each follows its language's
idioms (C++ RAII + `std::expected`, Rust ownership + typed `Result`, Zig
explicit allocators + `defer`). What they must **not** differ on is *behaviour*.
This page defines that behaviour once per subsystem; the per-language spelling is
shown side by side so a divergence is easy to spot.

The rule of thumb: **the contract is identical, the syntax is language-shaped.**
Where a genuine asymmetry exists (e.g. Rust has channels, C++/Zig do not), it is
called out explicitly rather than papered over.

A recurring difference is *how a scope-bound resource is released*:

- **C++** releases in a **destructor** (RAII) — automatic at scope exit.
- **Rust** releases in **`Drop`** — automatic at scope exit.
- **Zig** has no destructors, so it releases via an explicit **`defer
  guard.deinit()`** (or `.release()`). The release is just as deterministic; it
  is written, not implicit.

---

## Thread

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Spawn (cooperative) | `Thread<N>(entry, prio, name)` | `Thread::builder()…spawn(f)` / `spawn_cooperative(fn)` | `Thread(N).spawn(alloc, cfg, fn, args)` |
| Entry — no token | `void(void*)` | `spawn_simple(fn())` | `fn()` |
| Entry — cooperative | `void(stop_token)` | `FnOnce(StopToken)` / `fn(StopToken)` | first param `StopToken` |
| Capturing closure | heap mode only | heap `spawn` (`FnOnce`) | via context pointer arg |
| Zero-heap form | caller storage + stack | `spawn_static(&storage, …)` | embedded `Backing` (stack in handle) |
| Drop / deinit | `~Thread` → request-stop + join | `Drop` → request-stop + destroy | `deinit()` → request-stop + deinit |
| Opt out of join-on-drop | `detach()` (heap; deleted in zero-heap) | `detach()` | `detach()` |
| Idempotent deinit | yes | yes | yes |
| Cancellation flag | `stop_token::stop_requested()` | `StopToken::is_stopped()` | `StopToken.isStopped()` |
| Request stop | `Thread::request_stop()` | `Thread::request_stop()` | `Thread.requestStop()` |
| Current thread | `this_thread::self()` / `get_id()` (null handle on a thread oveRTOS did not create) | `Thread::current() -> Option<Thread>` (`None` there) | `getSelf()` (null there) |
| Priority | `ove_prio_t` (C enum) + `ove::prio_above`/`prio_below` | `Priority` newtype (`u8` level, anchor consts, `above`/`below`) | `Priority` non-exhaustive `enum(c_uint)` (`above`/`below`) |
| State | `ove_thread_state_t` (C enum) | `ThreadState` enum | `State` `enum(c_uint)` |

**Contract notes**

- A live thread handle **owns** the kernel thread. Letting the handle go out of
  scope requests cooperative stop and then joins/destroys it. `detach()` discards
  that owner without stopping the worker, so its oveRTOS handle and stack are not
  reclaimed. Reserve it for a bounded set of program-lifetime workers; otherwise
  retain the handle and shut the worker down cooperatively.
- Cooperative cancellation is the supported shutdown path: the entry loops
  `while (!stop_requested())` and the owner calls `request_stop()` (implicitly via
  drop, or explicitly).
- `Priority` is a level on the 0..31 scale in every binding. The named values
  are anchors on it (`OVE_PRIO_NORMAL`, `Priority::Normal`, `.normal`), and
  each binding steps between them the same way, clamping at both ends:
  `OVE_PRIO_ABOVE(p, n)` / `ove::prio_above(p, n)` / `p.above(n)` and the
  `below` forms. Rust keeps the anchor names as associated consts, so
  `Priority::Normal` reads as before; a level outside the scale cannot be
  built (`Priority::from_level` returns `None`, Zig's `fromLevel` `null`).
- `State` is a typed enum in Rust and Zig; C++ currently surfaces the raw C
  enum (`ove_thread_state_t`). Treat the named values as the contract, not
  the integers.

---

## Queue & Channel

`Queue<T, N>` is a bounded MPMC queue of trivially-copyable `T`. A **channel**
(`Sender`/`Receiver`) is a *Rust-only* refcounted layer over `Queue` that adds
peer-drop detection; **C++ and Zig have no channel type** — they use `Queue`
directly.

| Contract | C++ | Rust `Queue` | Zig | Rust `channel` |
|---|---|---|---|---|
| Blocking receive | `receive(T&)` (abort on fail) | `recv() -> Result<T>` | `recv() -> T` (infallible, forever) | `Receiver::recv() -> Result<T>` (`NetClosed` when senders gone) |
| Bounded-wait receive | `try_receive_for(T&, dur) -> Result<void>` | `try_recv_for(dur) -> Result<T>` | `recvFor(dur) -> …!T` | — |
| **Non-blocking receive** | `try_receive() -> Result<T>` (value); legacy `try_receive(T&) -> bool` | `try_recv() -> Result<T>` (`QueueEmpty`) | `tryRecv() -> ?T` | `try_recv() -> Result<T, TryRecvError>` |
| Send (bounded-wait) | `try_send_for(const T&, dur) -> Result<void>` | `try_send_for(dur) -> Result<()>` | `sendFor(dur) -> …!void` | `Sender::try_send(T) -> Result<()>` |
| Queue empty (non-blocking) | `Error::QueueEmpty` | `Error::QueueEmpty` | `null` | `TryRecvError::Empty` |
| Queue full | `Error::QueueFull` | `Error::QueueFull` | `error.QueueFull` | `Error::Timeout` / `Error::NetClosed` |
| Peer closed | n/a (no peers) | n/a (no peers) | n/a (no peers) | `NetClosed` (recv) / `TryRecvError::Disconnected` (try_recv) |

**Contract notes**

- **Non-blocking receive returns the item by value**, sharing one return channel
  with the empty/closed signal: C++ `Result<T>`, Rust `Result<T>`, Zig `?T`. The
  C++ `bool try_receive(T&)` out-param form is retained for back-compat and
  hot-path use, but new code should prefer the value form.
- The **Empty vs Disconnected** distinction only exists where there are peers to
  drop — i.e. the Rust channel. `Receiver::try_recv` returns
  `TryRecvError::Empty` while a sender is alive and `TryRecvError::Disconnected`
  once the queue is drained *and* every `Sender` is gone (mirrors
  `std::sync::mpsc`). A bare `Queue` is never "disconnected".
- Zig's forever-blocking `recv()` is intentionally infallible (`T`, not `!T`): a
  forever wait either returns an item or the program is mis-built.

---

## Mutex

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Wraps the guarded data? | no (bare lock) | **yes** — `Mutex<T>` over `UnsafeCell<T>` | no (bare lock) |
| Blocking lock | `lock()` (abort on fail) | `lock() -> Result<MutexGuard<T>>` | `lock()` (panic on fail) |
| Non-blocking try | `try_lock() -> bool` | `try_lock() -> Result<MutexGuard<T>>` (`WouldBlock`) | `tryLock() -> bool` |
| Bounded-wait try | `try_lock_for/until() -> Result<void>` | `try_lock_for/until() -> Result<MutexGuard<T>>` | `lockFor/lockUntil() -> LockError!void` |
| Scoped guard | `LockGuard` / `std::lock_guard<ove::Mutex>` | `MutexGuard` (from `lock()`) | `Guard` (from `acquire()`) |
| Guard releases via | destructor (RAII) | `Drop` (RAII) | explicit `guard.release()` / `defer` |
| Recursive variant | `RecursiveMutex` | `RecursiveMutex` | `RecursiveMutex` |
| Idempotent deinit | yes | yes | yes |

**Contract notes**

- Rust folds lock + data into `Mutex<T>` (the guard `Deref`s to `T`, and is
  `!Send`); C++ and Zig are bare locks guarding caller-held data. All three
  release the lock deterministically at scope end — automatically in C++/Rust,
  via `defer guard.release()` in Zig.
- `try_lock` reports contention as `false` (C++/Zig) or `Err(WouldBlock)` (Rust);
  the bounded-wait forms report expiry as `Error::Timeout` / `error.Timeout`.

---

## Timer

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Create | `Timer(cb, user, period_ms, one_shot)` | `Timer::new(cb, period_ms, one_shot)` / `from_static` | `Timer.create(alloc, cfg, cb, args)` |
| Mode | `one_shot` bool | `one_shot` bool | `cfg.mode = .periodic / .one_shot` |
| Callback shape | `ove_timer_fn` | `fn()` | `fn()` / `fn(*Ctx)` |
| start / stop / reset | `Result<void>` | `Result<()>` | `Error!void` |
| Drop / deinit | `~Timer` → destroy/deinit | `Drop` → destroy/deinit | `deinit()` → deinit |
| Idempotent deinit | yes | yes | yes |

**Contract notes**

- A timer is created **stopped**; `start()` arms it. Dropping the handle stops
  and destroys the underlying timer. `reset()` restarts the period from now.
- Dropping or deinitialising the handle waits for a callback that is running,
  so none of the three may do it from the timer's own callback.

---

## Callbacks

Every binding hands the C layer a trampoline, so the context a callback runs in,
and how long its data must live, are the C contract. Each wrapper's docs must
state it the same way, including where an engine does **not** guarantee it.

| Callback | Runs in | After unregister / stop / deinit returns |
|---|---|---|
| Thread entry | the new thread; returning ends it | deinit / destroy waits for the return |
| Timer | thread context: FreeRTOS timer task, Zephyr system work queue, NuttX HPWORK, host dispatcher thread | stop: one running or queued call may still run (FreeRTOS: more, if the caller outranks the timer task); deinit / destroy waits for a running call |
| GPIO interrupt | interrupt context on every engine (host: the injecting thread) | disable / unregister from thread context: no call starts or is running (single core); not guaranteed from a higher-priority interrupt |
| Work item | the work queue's thread, one item at a time | `ove_work_cancel` / deinit drain it |
| PM policy | the engine's low-priority PM thread, without the PM lock, with the `user_data` registered alongside it | a call already started may run once more |
| PM notifier | the PM thread, with the PM lock held | unregister waits for a running call; the notifier must not call other PM functions |
| Audio node / tap | the thread driving the graph (audio thread, or the caller of `process`) | stop / deinit wait for a running cycle and join the device sink's thread (never call them from a callback); not guaranteed today with the native Zephyr, NuttX and FreeRTOS-on-STM32 drivers |
| httpd handler / WebSocket | the single `httpd` thread, one at a time | stop waits for that thread; WebSocket `on_close` is not called for connections open at stop or dropped by a failed send |
| Shell command | the thread that feeds `process_char` / `process_line` | — (never feed the shell from an ISR) |
| Protected task | the caller's thread, synchronously | a contained fault abandons it by longjmp |

**Contract notes**

- A binding that wraps a callback in a safe API must not promise more than this
  table does. Where a row says "not guaranteed today", the wrapper documents
  the gap rather than hiding it (the audio `stop` / `deinit` docs in every
  binding do for the native drivers).
- Strings a callback API keeps by pointer (shell command names and help, thread
  names on NuttX, Zephyr and the host backend) must outlive the registration:
  Rust takes `&'static CStr`, Zig comptime or caller-owned names, C++ documents
  the rule.

---

## LVGL

LVGL is a C library; the bindings provide **thin, non-owning** wrappers plus a
lock for thread safety. The wrappers do not own the underlying `lv_obj_t` — they
are value-sized views; deleting the widget tree is an explicit `.del()`.

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Thread-safety lock | `LvglGuard` (RAII) | `lock() -> LvglGuard` (`Drop`) | `lock() -> LvglGuard` (`defer .deinit()`) |
| `init()` | `int` (raw rc) | `Result<()>` | `Error!void` |
| `tick(ms)` / `handler()` | `void` | `void` | `void` |
| Widget create | `Label::create(parent)` | `Label::create(parent)` | `Label.create(parent)` |
| Chaining | CRTP mixins | blanket traits (`Layout`/`Styleable`/…) | comptime mixin fns |
| Ownership | non-owning view | non-owning `Copy` | non-owning value |
| Delete tree | `.del()` | `.del()` | `.del()` |
| Typed constants | `LV_ALIGN_*` / `LV_PALETTE_*` (C macros) | `lvgl::ALIGN_*` / `Palette` | `lvgl.ALIGN_*` / `lvgl.PALETTE_*` |
| Raw escape hatch | implicit `lv_obj_t*` / `handle()` | `Widget::raw()` + `bindings::` | `obj` field + `ove.ffi` |

**Contract notes**

- The graphics thread must hold the LVGL lock around `tick`/`handler` and any
  widget mutation. C++ and Rust release the lock at scope exit (destructor /
  `Drop`); Zig uses `const g = lvgl.lock(); defer g.deinit();`.
- **`init()` is the one place the return shape legitimately differs.** Rust and
  Zig return their typed result; C++ returns a plain `int` rc *on purpose*:
  `lvgl.hpp` must stay **C++20-compatible** (the `test-lvgl-compile` gate compiles
  it pre-C++23), and `ove::Result` is `std::expected`, which is C++23. The
  contract is "non-zero/negative means failure" in all three.
- Prefer the typed `ALIGN_*` / `PALETTE_*` constants over the raw C macros. The
  escape hatch exists for genuinely unwrapped calls (e.g. the 3-byte `lv_color_t`
  return ABI on ARM), which should carry a one-line comment explaining why.

---

## Error

All fallible `ove_*` C calls return a negative `OVE_ERR_*` code; each binding
lowers that into its language's error type.

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Error type | `enum class Error` | `enum Error` (`#[non_exhaustive]`) | `Error` error set |
| Success/failure carrier | `Result<T>` = `std::expected<T, Error>` | `Result<T>` = `Result<T, Error>` | `Error!T` |
| Unknown / unmapped code | — (maps the pinned set) | `Error::Unknown(i32)` (carries the code) | `Error.UnknownErrorCode` in release; **panics in `Debug`** |
| Code/enum pin | `static_assert` | `const` assertions | `comptime` asserts |

**Contract notes**

- The numeric `OVE_ERR_*` ↔ enum mapping is **pinned** in every binding and
  enforced by the `error-code-pin` lint (`make lint`): every C code must be
  handled by every binding.
- Unknown-code policy: Rust always surfaces `Error::Unknown(code)` (it can carry
  the integer). Zig cannot carry a payload in an error set, so it **panics in
  `Debug`** (loud, localised drift detection during development/tests) and
  **degrades to `Error.UnknownErrorCode` in release** so a shipped binary never
  hard-crashes on a code the substrate added but the binding hasn't mapped yet.

---

## Maintaining this contract

When you add or change a binding API, update this page in the same change so the
three languages stay aligned. The examples under `apps/{cpp,rust,zig}/` act as
conformance tests for these semantics — if an example has to drop to raw FFI to
express something, that is a gap in the contract (or the wrapper), not a licence
to diverge.
