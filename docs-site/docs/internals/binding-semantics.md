# Binding Semantics

!!! note "Audience"
    This page is for **oveRTOS developers** writing or reviewing the C++, Rust
    and Zig bindings (`bindings/{cpp,rust,zig}/ove/`). It defines what each
    wrapper must do. App developers can read the [API Reference](../api/index.md)
    instead; this page keeps the three bindings behaviourally identical.

Each binding follows its language's idioms (C++ RAII + `std::expected`, Rust
ownership + typed `Result`, Zig explicit allocators + `defer`). They must not
differ on *behaviour*. **The contract is identical, the syntax is
language-shaped.** A genuine asymmetry (e.g. Rust has channels, C++/Zig do not)
is called out explicitly.

A recurring difference is *how a scope-bound resource is released*:

- **C++** releases in a **destructor** (RAII) at scope exit.
- **Rust** releases in **`Drop`** at scope exit.
- **Zig** has no destructors and releases with an explicit **`defer
  guard.deinit()`** (or `.release()`). It is just as deterministic.

Rust-only ownership rules (typed static storage claims, context admission,
async setup) are stated per subsystem below. They add no C++ or Zig API.

---

## Timeouts

Relative synchronization and IPC waits use the native engine budget in C, C++,
Rust and Zig. Timeouts and stream short counts refer to this effective budget.
Only explicit FOREVER waits are indefinite; tick phase, scheduling, composed
waits and condition-variable mutex reacquisition can extend elapsed time.

- **FreeRTOS** synchronization, queue, EventGroup, stream and DNS-completion
  waits convert a positive finite budget with exact `ceil(ns * Hz / 1e9)`,
  saturating at `portMAX_DELAY - 1` before narrowing to the tick type. FOREVER
  selects `portMAX_DELAY`, which waits indefinitely only with
  `INCLUDE_vTaskSuspend=1`. A zero DNS timeout selects a ten-second default;
  the DNS serialization mutex wait is outside the budget.
- **NuttX** rounds positive finite waits up to ticks and saturates at
  `min(WDOG_MAX_DELAY - 2, INT32_MAX - 1)` ticks.
- Other engines use their own DNS timeout policies.

---

## Strings passed to C

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Transient core text (paths, keys, routes, metadata, TLS hostname) | `const char *` | `&str` / `Option<&str>`; FS also accepts byte paths | `[:0]const u8` / `[*:0]const u8` |
| Retained audio names | persistent C string adapted to a span | `&'static str`, retained counted span | persistent sentinel string adapted to a span |
| Retained shell/thread names | persistent C string | existing `&'static CStr` contract | persistent sentinel string |
| Copied LVGL text, image paths and symbols | `const char *` | `&str`, checked and converted privately | sentinel string |
| Retained LVGL label text | caller-owned persistent buffer | static `TextStorage<N>` + `BufferedLabel<N>` | caller-owned sentinel buffer |
| Reactive LVGL label formatting | persistent printf format | noncapturing Rust formatter function | persistent printf format |
| Built at run time, no heap | `snprintf` into a buffer | selected `_fmt(format_args!(...))` methods | `std.fmt.bufPrintZ` |
| Data with an explicit length (bodies, payloads, NVS values) | pointer + length | `&[u8]` | `[]const u8` |

**Contract notes**

- Rust privately terminates bounded C-string inputs (FS paths 255 bytes, TLS
  hostname 255, httpd method 7 / path 63) and passes counted inputs to NVS,
  response and log consumers by length. A byte slice is never handed to a
  consumer that reads to a terminator. Embedded NUL is rejected before any
  effect. FS byte paths keep non-UTF-8 names. The native `_cstr` variants
  remain available.
- Checked formatting runs once, latches errors and publishes no partial key or
  path. Core staging needs neither LVGL nor a Rust allocator.
  `FmtBuf::as_c_str()` is an optional low-level utility.
- Audio retains static `name_span { data, len }` bytes: no terminator, no
  name cap and no allocation. Readers must use the length.
- Rust LVGL string methods return `Result`: embedded NUL is `InvalidParam`,
  conversion pool exhaustion is `NoMemory`. Short conversions use stack
  scratch, longer ones LVGL's fixed pool; no Rust global allocator is needed.
  `set_text_fmt(format_args!(...))` and `cell_value_fmt` keep the old text on
  preparation errors; LVGL's own void setters follow its assertion policy for
  internal allocation failures.
- `Label::text_buffered` attaches a static `TextStorage<N>` once. Its
  `BufferedLabel` updates allocate no memory; capacity includes the NUL.
  Overflow returns `NoSpace` without truncating UTF-8 or changing the text.
  Replacement and deletion invalidate old tokens; the storage can then be reused.
- `Label::bind_text` accepts `fn(i32, &mut dyn core::fmt::Write) -> fmt::Result`.
  The binding owns the observer context until replacement, deletion or subject
  teardown. `text_status()` reports asynchronous formatting errors, and
  `NotFound` when unbound. A recursive state change that invalidates a render
  reports `Busy` unless LVGL dispatches a fresh render of the latest value.
- A `State` must keep its address while observers are attached (C++ `State`
  is non-copyable, Rust bind methods take `Pin<&State<T>>`, and Zig documents
  this requirement), and dropping or deinitialising it detaches the observers.
- All LVGL operations, including conversion and storage updates, require
  initialized LVGL and its lock. Callbacks already holding the lock must not
  reacquire it. Image strings are borrowed only until the widget copies them;
  image descriptors and pixels must stay alive.
- Rust `ove::net_httpd::ws::send` is `unsafe`: its connection handle is a raw
  pointer that stops being valid once the connection's close callback runs.

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
| Suspend / resume | `suspend()` / `resume()` → `Result<void>` | `suspend()` / `resume()` → `Result<()>` on `Thread` and `JoinHandle` | `suspendThread()` / `resumeThread()` → `Error!void` |
| Current thread | `this_thread::self()` / `get_id()` (null handle on a thread oveRTOS did not create, or from an ISR) | `Thread::current() -> Option<Thread>` (`None` there) | `getSelf()` (null there) |
| From an ISR | `thread_list` → `Error::WouldBlock`; sleeps return at once | `spawn_cooperative` / `spawn_simple` / `spawn_static[_simple]` / `thread_list` → `Error::WouldBlock`; `sleep_ms` returns at once | `threadList` → `error.WouldBlock`; `sleepMs` returns at once |
| Priority | `ove_prio_t` (C enum) + `ove::prio_above`/`prio_below` | `Priority` newtype (`u8` level, anchor consts, `above`/`below`) | `Priority` non-exhaustive `enum(c_uint)` (`above`/`below`) |
| State | `ove_thread_state_t` (C enum) | `ThreadState` enum | `State` `enum(c_uint)` |

**Contract notes**

- A live thread handle **owns** the kernel thread. Dropping it requests
  cooperative stop, then joins/destroys. `detach()` discards the owner without
  stopping the worker, so its handle and stack are never reclaimed. Reserve it
  for a bounded set of program-lifetime workers.
- Cooperative cancellation is the supported shutdown path: the entry loops
  `while (!stop_requested())` and the owner calls `request_stop()` (implicitly
  via drop, or explicitly).
- Suspend and resume pass through the [C thread contract](../api/threads.md).
  NuttX reports `NotSupported` for both calls and never reports the suspended
  state. Empty C++ and Zig thread wrappers report `InvalidParam`. Zephyr
  allows ISR calls, while FreeRTOS and the POSIX host backend require thread
  context.
- From an ISR the C thread init/deinit/create/destroy/list calls return
  `OVE_ERR_WOULD_BLOCK`, `ove_thread_get_self` returns NULL and millisecond
  sleeps and delays return at once. The bindings' list, current-thread and
  sleep calls pass this through. Spawning and owner teardown are task-context
  only: Rust heap `spawn` and Zig `spawn` allocate first, C++ construction
  asserts on the error, and C++/Rust/Zig teardown ignores it.
- Native pthread workers have no oveRTOS identity even when an oveRTOS task
  created them; they do not inherit its stop flag or state attribution.
- `Priority` is a level on the 0..31 scale in every binding. The named values
  are anchors on it (`OVE_PRIO_NORMAL`, `Priority::Normal`, `.normal`); every
  binding steps between them the same way, clamping at both ends:
  `OVE_PRIO_ABOVE(p, n)` / `ove::prio_above(p, n)` / `p.above(n)` and the
  `below` forms. A level outside the scale cannot be built
  (`Priority::from_level` returns `None`, Zig's `fromLevel` `null`).
- `State` is a typed enum in Rust and Zig; C++ surfaces the raw C enum. Treat
  the named values as the contract, not the integers.

---

## Queue & Channel

`Queue<T, N>` is a bounded MPMC queue of trivially-copyable `T`. A **channel**
(`Sender`/`Receiver`) is a *Rust-only* refcounted layer over `Queue` that adds
peer-drop detection; **C++ and Zig have no channel type** and use `Queue`
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
| Occupancy (snapshot) | `size()` / `size_from_isr()` / `capacity()` / `empty()` / `full()` | `len()` / `len_from_isr()` / `capacity()` / `is_empty()` / `is_full()` | `len()` / `lenFromIsr()` / `capacity()` / `isEmpty()` / `isFull()` | — |

**Contract notes**

- **Non-blocking receive returns the item by value**, sharing one return channel
  with the empty/closed signal: C++ `Result<T>`, Rust `Result<T>`, Zig `?T`. The
  C++ `bool try_receive(T&)` out-param form remains for hot paths; new code
  should prefer the value form.
- **Empty vs Disconnected** exists only where there are peers to drop: the Rust
  channel. `Receiver::try_recv` returns `TryRecvError::Empty` while a sender is
  alive and `TryRecvError::Disconnected` once the queue is drained *and* every
  `Sender` is gone (as `std::sync::mpsc`). A bare `Queue` is never disconnected.
- Zig's forever-blocking `recv()` is infallible (`T`, not `!T`): a forever wait
  either returns an item or the program is mis-built.
- Native initialization validates nonzero dimensions, size overflow and provider
  limits before entering the kernel. A partial failure retires earlier resources
  and publishes no handle, so the same quiescent backing can be retried. Rust
  and Zig propagate the error; C++ applies its constructor assertion after
  native cleanup. C callers keep storage and the item buffer exclusive through
  teardown.
- Queue-only configurations provide `ove_is_in_isr` and `ove_sync_context`
  without SYNC, TIME or ASYNC.

**Rust ownership and context**

- Typed zero-heap backing stays claimed until native cleanup completes; while
  an owner is live, initializing, tearing down, forgotten or retained, another
  claim returns `Busy` before native entry. A clean initialization error
  reopens it. Heap owners are independent; raw owners deinit their backing.
- Payloads are invariant, so shared insertion cannot shorten a stored
  reference's lifetime. Owner `Send`/`Sync` requires `T: Copy + Send`.
- Construction, destruction and notification setup require RESOURCE (an
  unavailable Drop retains the resource and claim); task transfers, including
  zero timeouts, require BLOCKING before any native or clock effect. Outside
  admitted context occupancy snapshots read zero (`is_empty` true, `is_full`
  false), which is not actual emptiness. ISR-named methods called from a task
  take the task route. Relative durations work without TIME; absolute
  deadlines need it.
- `AsyncQueue::arm` and `AsyncSemaphore::arm` take exclusive `&'static mut`
  access and return the shared `&'static` reference only after installing the
  wake callback: initialize at the final address (`StaticCell` works), arm
  once, then publish the reference. `AsyncQueue` needs RESOURCE for this,
  `AsyncSemaphore` PRODUCER. An error still consumes the borrow, so safe code
  cannot retry or reclaim. The single waker slot gives no fairness between
  waiters.

---

## Stream

A stream moves bytes, so its calls report how many moved rather than failing
when a full stream or a timeout moved fewer than asked. The wrappers pass the C
result through; the C contract (see the [IPC reference](../api/ipc.md)) is that
only an invalid call is an error.

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Blocking send / receive | `send` / `receive` (abort on fail) | `send` / `recv -> Result<usize>` | `send` / `recv -> usize` (panic on fail) |
| Non-blocking | `try_send` / `try_receive -> bool` + count | `try_send` / `try_recv -> Result<usize>` | `sendFor` / `recvFor` with a zero duration |
| Bounded wait | `try_send_for/until` / `try_receive_for/until -> Result<void>` + count | `try_send_for/until` / `try_recv_for/until -> Result<usize>` | `sendFor/Until` / `recvFor/Until -> …!usize` |
| From an interrupt | `send_from_isr` / `receive_from_isr -> Result<void>` + count | `send_from_isr` / `receive_from_isr -> Result<usize>` | `sendFromIsr` / `receiveFromIsr -> …!usize` |
| Full stream, timeout, nothing there | success, count 0 or short | `Ok(0)` or short | 0 or short |
| Empty buffer (`len` 0) | success, count 0, never waits | `Ok(0)`, never waits | 0, never waits |

**Contract notes**

- A receive waits for the stream's trigger level, then takes up to the buffer's
  length; the interrupt receive takes whatever is there.
- An empty buffer never waits, even on a forever wait, and a NULL pointer with a
  length of 0 is accepted, so an empty C++ `std::vector`'s `data()` is fine.
- Zig's `SendError` / `RecvError` (`error.Timeout`) and the async `read`'s
  `WouldBlock` / `Timeout` arms are never produced by the C layer today.

---

## Mutex

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Wraps the guarded data? | no (bare lock) | **yes** — `Mutex<T>` over `UnsafeCell<T>` | no (bare lock) |
| Blocking lock | `lock()` (abort on fail) | `with_lock(f) -> Result<R>` | `lock()` (panic on fail) |
| Non-blocking try | `try_lock() -> bool` | `try_with_lock(f) -> Result<R>` | `tryLock() -> bool` |
| Bounded-wait try | `try_lock_for/until() -> Result<void>` | `try_with_lock_for/until(..., f) -> Result<R>` | `lockFor/lockUntil() -> LockError!void` |
| Scoped access | `LockGuard` / `std::lock_guard<ove::Mutex>` | nonowning `MutexAccess` inside callback | `Guard` (from `acquire()`) |
| Native release | guard destructor | private binding lease at callback end | explicit `guard.release()` / `defer` |
| Recursive variant | `RecursiveMutex` | `RecursiveMutex` | `RecursiveMutex` |
| Idempotent deinit | yes | yes | yes |

**Contract notes**

- Rust's nonowning `MutexAccess` provides `Deref`/`DerefMut`, is `!Send` and
  cannot escape the callback. Dropping or forgetting it leaves the private
  release intact; to release early, end the callback. `RecursiveMutex` has
  matching no-data callback methods and no public unlock.
- Rust callbacks return `ove::Result<R>`. For a distinct application error, map
  it explicitly or return `Ok(app_result)` and use `??` at the call site.
- Native contention or expiry maps to a Rust error (currently `Timeout`;
  same-thread recursive entry to an ordinary data mutex is `Busy`). C++/Zig
  boolean try methods return false on contention.

---

## Synchronization primitives

**Construction and teardown.** The five native synchronization families publish
a handle only after complete initialization. A failure leaves the output
untouched and releases the pieces already initialized, so the backing can be
retried or freed. Teardown requires a successfully initialized, quiescent
object: no waiters, active operations or outstanding mutex acquisitions.

C++ constructors and void acquisition/wait methods invoke the configured
assertion on error and abort if it returns, including in release builds.
Boolean and `Result` methods stay fallible. Rust constructors return errors;
Zig constructors propagate errors and free their allocated storage, and its
void acquisition methods panic.

**Semaphore counts** require `max > 0`, `initial <= max` and the provider
ceiling: `SEM_VALUE_MAX` on POSIX/NuttX, `UBaseType_t` on FreeRTOS and
`unsigned int` on Zephyr. FreeRTOS and Zephyr enforce the requested maximum.
POSIX and NuttX enforce only their provider-wide ceiling, in every binding.

**Condition variables.** `ove_condvar_wait_checked` reports on every return
whether the caller still owns its ordinary mutex: held after success or clean
timeout, either way after other errors. The output must be writable and
disjoint from the primitive. Rust uses it to keep its private cleanup correct
and returns reacquired access only on success or clean timeout; C++ and Zig
convenience methods use the plain status. Waiter retirement finishes before
return; a permanently broken internal mutex has no bounded recovery.

**Rust context admission.** `Mutex`, `RecursiveMutex`, `Semaphore`, `Event`
and `CondVar` check `ove_sync_context` before resource operations, waits and
deadline-clock reads (see [IRQ and context queries](#irq-and-context-queries)):

- Unsupported `Result` methods return `NotSupported` before native entry,
  including zero-time waits; `CondVar` signal/broadcast do nothing there.
  Releasing an already-owned mutex still works with interrupts masked.
- Semaphore releases outside PRODUCER admission have no effect, including in
  an ISR. The Event producer methods take the actual task or ISR route; native
  ISR setup must meet its kernel-aware priority contract.
- Typed zero-heap storage (used by all five creation macros) is claimed before
  native init, rejects a duplicate live init with `Busy`, reopens only after
  cleanup and stays reserved when its owner is forgotten or its teardown is
  unavailable; `T` is still dropped or extracted exactly once. Heap
  construction ignores it. Raw backing must stay stable through cleanup or
  retention.
- A recursive mutex admits at most 65,535 combined held and pending
  acquisitions.

`Semaphore::set_notify` requires PRODUCER admission and is `unsafe`:
registration must be serialized with every native give and other
registration, the old callback and data must outlive in-flight calls, and the
callback must tolerate concurrent calls from all release callers. Native gives
call the hook after every post attempt, even when the count ceiling prevents
an increment.

**Rust `SpinMutex`** gives scoped `with(f)` access: one exclusive data loan and
a private native IRQ key, both released when the callback returns or unwinds.
Re-entering the same object restores its key and panics; different objects can
nest and retire in reverse order. A terminal panic or abort runs no unwind
cleanup. The callback's result passes through unchanged and no reference or
key escapes it. `new` is const; `get_mut`/`into_inner` give exclusive-owner
access. It needs native SYNC plus ASYNC (Cargo `async` selects the runtime).
Keep callbacks short and nonblocking; the
[critical-section](#irq-and-context-queries) rules apply.

---

## EventGroup

- Native initialization publishes a handle only after every stage succeeds. A
  failure releases earlier stages and returns the initialization error; heap
  helpers free failed allocations. This covers C and the C++, Rust and Zig
  constructors. `ove_sync_context` is available with EVENTGROUP alone.
- Masks use the configured payload domain from the `valid_mask` accessor in
  every binding: FreeRTOS reserves its native high byte, other engines expose
  all 32 bits. Invalid scalar masks return zero without effects; ISR clear and
  invalid waits return InvalidParam. Waits reject zero masks and unknown flags
  before touching the result or a deadline clock. Zero set/clear masks are
  valid. Task clear returns the full prior value.
- NuttX snapshots share writer exclusion. Zephyr clear shares the native event
  lock through a private syscall that keeps granted user-mode callers working.
- Zig timed waits return InvalidParam alongside Timeout; its forever wait
  requires valid inputs and panics on an unexpected error.
- FreeRTOS deferred commands use stable internal tokens. Teardown cancels queued
  work before native deletion and releases the backing at once; external
  sources, producers and synchronous users must already be quiescent. The timer
  daemon protects its update against task teardown, drops owner/token access,
  then runs saved notify callbacks after publication; clear propagates its wake
  request. MPU V1 teardown keeps caller privilege. Notify code and data must
  stay live through pending dispatch: unregister/deinit is not a reclamation
  fence.

**Rust ownership and context**

- Typed storage claims one native generation until legal cleanup completes,
  with the same live/forgotten/retained exclusion as `Queue`; the typed
  eventgroup macro uses it. Raw constructors need stable exclusive backing.
- Construction, cleanup and notify setup require RESOURCE; task operations,
  including zero waits, require BLOCKING. Unsupported scalar calls return zero
  and `Result` methods `NotSupported`, before any effect; an unsupported Drop
  retains the resource and claim. ISR-named calls made by a task use the task
  operations. Relative waits work without TIME; `Instant` methods need it.
- `AsyncEventGroup::arm` takes a shared reference; it has no exclusive-setup
  guarantee like `AsyncQueue`'s.

---

## IRQ and context queries

| Query | Available with | Result |
|---|---|---|
| `ove_is_in_isr()` | always | true in interrupt context (host: inside simulator ISR wrappers) |
| `ove_sync_context()` | SYNC, QUEUE or EVENTGROUP | mask of `OVE_SYNC_CONTEXT_RESOURCE` / `_BLOCKING` / `_PRODUCER`; zero when disabled |
| `ove_irq_lock()` / `ove_irq_unlock()` | ASYNC | native exclusion; disabled stubs give none |

None of them needs object setup, allocation, TIME or ASYNC (except the lock).

**Context flags** (full rules on `ove_sync_context` in `ove/irq.h`):

- RESOURCE (native init/destroy) and BLOCKING (mutex acquisition, condition
  variables) need an active, non-idle thread with interrupts enabled; FreeRTOS
  also grants RESOURCE before the scheduler starts.
- PRODUCER (task semaphore/Event posts, serialized notification setup) also
  holds in masked task contexts and nested critical sections, and on FreeRTOS
  before scheduler start and while the scheduler is suspended. It permits
  nothing else.
- All flags are zero in an ISR, in unprivileged code, on unsupported
  architectures, FreeRTOS multicore and NuttX SMP. They are not the test for
  releasing an already-owned mutex, which can stay legal with interrupts masked.
- POSIX timer dispatchers mark callbacks as ISR context and its critical
  sections report PRODUCER only; this does not model signal handlers or real
  IRQ exclusion.

**Critical sections.** Every binding shares the native domain of
`ove_irq_lock`/`ove_irq_unlock`; Rust's global `critical_section` provider and
`SpinMutex::with` do not widen it. Pass the opaque 64-bit key back unchanged,
once, in LIFO order on the same thread or handler activation (FreeRTOS task
keys need not be zero). No binding-owned heap or application FFI is needed.
The FreeRTOS admission rules (supported ports, eligible handlers and grouping,
MPU V1 restricted tasks, stop-before-key even with assertions disabled) and the
native-body limitation (unlock restores the captured masks, but native calls in
the body can lower a stronger BASEPRI or clear PRIMASK, so this is not a global
IRQ mask) are stated on `ove_irq_lock` and in the
[IRQ contract](../api/sync.md#irq-critical-sections). Other engines keep their
native contracts.

---

## Timer

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Create | `Timer(cb, user, period_ms, one_shot)` | `Timer::new(cb, period_ms, one_shot)` / `from_static` | `Timer.create(alloc, cfg, cb, args)` |
| Mode | `one_shot` bool | `one_shot` bool | `cfg.mode = .periodic / .one_shot` |
| Callback shape | `ove_timer_fn` | `fn()` | `fn()` / `fn(*Ctx)` |
| start / stop / reset | `Result<void>` | `Result<()>` | `Error!void` |
| Period (as requested) | `period() -> std::chrono::nanoseconds` | `period() -> Duration` | `period() -> Duration` |
| Drop / deinit | `~Timer` → destroy/deinit | `Drop` → destroy/deinit | `deinit()` → deinit |
| Idempotent deinit | yes | yes | yes |

**Contract notes**

- A timer is created **stopped**; `start()` arms it. Dropping the handle stops
  and destroys the underlying timer. `reset()` restarts the period from now.
- Dropping or deinitialising the handle waits for a callback that is running,
  so none of the three may do it from the timer's own callback.

---

## Work queue

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Submit (thread context) | `work.submit(wq)` | `work.submit(&wq)` | `wq.submit(&work)` |
| Submit from an interrupt | `work.submit_from_isr(wq)` | `work.submit_from_isr(&wq)` | `wq.submitFromIsr(&work)` |
| Already pending or running | `Error::Busy` | `Error::Busy` | `error.Busy` |
| No room (interrupt submit) | `Error::QueueFull` | `Error::QueueFull` | `error.QueueFull` |

**Contract notes**

- The interrupt submit never waits; the handler still runs on the queue's
  thread. There is no interrupt-safe delayed submit or cancel.

---

## SPI and I2C

The native rules (byte counts and provider limits, 16-bit frames and alignment,
buffer overlap, per-segment timeouts, chip-select and mutex scope, read filler,
seven-bit addresses, repeated starts, async completion context and retention)
are defined once in the [SPI](../api/spi.md) and [I2C](../api/i2c.md)
references. They are checked before any effect and apply unchanged through
every binding; I2C transfers are never split to bypass a limit. C++ and Zig
pass them through. Rust adds owners, documented with their examples in those
references:

- `Spi`/`Spi16`, `I2c` and their `AsyncSpi*`/`AsyncI2c` forms take ordinary
  `Config`/`ChipSelect`/`Transfer` values and need no application FFI, Rust
  global allocator or manual pinning. Heap owners use the native allocator;
  static `*Storage` types are claimed until native deinit finishes, and a live
  or forgotten owner makes another init return `Busy`. Owners are not
  `Copy`/`Clone`/`Send`/`Sync`; moving one leaves native backing in place.
- Async owners stage TX/RX in binding-owned buffers. Static capacity overflow
  returns `NoSpace` before I/O. Only a successful live future copies RX back.
  Dropping or forgetting a future does not cancel the I/O: new nonempty work
  returns `BusBusy` until it settles, and `flush().await` consumes its result.
  Owner Drop defers cleanup until native callback, wake and submission tails
  finish; a missing completion retains bounded backing or a claim.
- The `embedded-hal`/`embedded-hal-async` adapters validate the whole operation
  list before effects and reject unsupported shapes.
- Other handles on the same bus, and raw async users, need external
  coordination.

---

## Inference

The C session rules are in the [inference API](../api/infer.md) and the
[trusted-model and arena contract](inference-runtime.md): one session is
admitted at a time across handles (a second init/create returns `OVE_ERR_BUSY`
before any effect, admission is held through teardown, and a session lost
without closing keeps it); models are trusted, immutable, 16-byte-aligned
build-time assets with no general validator; operations on one handle are
serialized by the caller; model bytes, storage and arena outlive the final
close; a handle cannot be reused once its storage holds another session.

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Session, heap-allocated (heap mode) | `ove::Model<> m(cfg)` | `Model::new(&cfg)` | `Model.create(&cfg)` |
| Reusable static storage + arena | `static ove::ModelStorage<N> s` | `ModelStorage::<N>::new()` (static) | `var arena: [N]u8 align(16)` |
| Load a model into it | `auto m = s.load(data, size)` | `let mut m = s.load(&cfg)?` | `try m.init(&arena, &cfg)` (zero-heap only) |
| Session end | destructor | `Drop` | `defer m.deinit()` |
| Arena usage including front padding | `m.arena_used()` | `m.arena_used()` | `m.arenaUsed()` |
| Detected allocation/planner exhaustion | `Error::NoMemory` | `Error::NoMemory` | `error.OutOfMemory` |
| Model alignment mismatch | `Error::InvalidParam` | `Error::InvalidParam` | `error.InvalidParam` |
| Schema mismatch | `Error::NotSupported` | `Error::NotSupported` | `error.NotSupported` |
| Unsupported tensor descriptor type or rank above five | `Error::NotSupported` | `Error::NotSupported` | `error.NotSupported` |
| Other setup failure | `Error::MlFailed` | `Error::MlFailed` | `error.MlFailed` |

**Contract notes**

- Below the startup minimum, or once allocation/planning capacity is exhausted,
  setup returns NoMemory (OutOfMemory in Zig); configurations in between are
  undefined. Each setup clears the arena in O(arena_size). Size the arena from
  a measured `arena_used` plus 16 bytes on the real target and re-measure after
  model, provider or TFLM changes. `ove_model_arena_used()` returns zero for
  null or closed handles; a dangling handle is invalid.
- Descriptors keep the exact float32, int8, uint8, int16 or int32 type and rank
  zero to five; unused dimensions are zero. C getters leave the descriptor
  unchanged on error, C++ typed pointer helpers return `nullptr`, Rust and Zig
  propagate the error. Outputs are read-only.
- C++ embedded `Model<N>`, Rust `ModelStorage` and Zig zero-heap `init` reject
  a configured arena larger than their storage before native effects and pass
  smaller sizes through unchanged. C++ `ModelStorage<N>::load` always uses N.
- The arena lives in caller storage meant to be static, never inside an object
  that may land on a thread's stack. A zero-heap C++ `ove::Model<N>` with
  `N > 0` still embeds its arena; prefer `ModelStorage`.
- One session at a time: a pipeline loads its models into one storage in turn,
  ending each session before the next.
- Asymmetry: Zig has no heap-mode form that borrows caller storage; its heap
  build allocates per session with `Model.create`.

**Per binding**

- C++ zero-heap `Model<N>` keeps the constructor assertion policy; if the hook
  returns, the model is empty with a null handle. Heap models allocate the
  requested size regardless of N. `ModelStorage<N>::load` returns errors
  through `Result`.
- Zig `ove.infer.linkedModel("g_my_model_data")` references the generated
  array and its `unsigned int` length; the integration guarantees a complete,
  immutable, aligned array with program lifetime (`modelSlice` is the raw
  adapter). Zero-heap `init` publishes state only after native success, so a
  failed attempt, including `Busy`, leaves an existing owner untouched. The
  wrapper must not move while live (checked in Debug); a repeated close is a
  no-op.
- Rust puts the qualification obligations on `unsafe
  ModelConfig::from_trusted_model` and `unsafe Model::invoke`, and on the
  generated `model_data!` accessor. Sessions borrow the model bytes and their
  storage exclusively. Tensor views accept only `f32`, `i8`, `u8`, `i16` and
  `i32` of the exact descriptor type and are bounds- and alignment-checked.
  Rust uses `ove_is_in_isr()` to detect ISR context, including inside host POSIX
  ISR wrappers and timer callbacks. Construction and invocation there return
  `InvalidParam`. Drop there retains the resources and admission permanently,
  so later construction returns `Busy`. The `ove::infer` crate docs hold the
  full rules.
- `ove_model_binding_arena` reports the live span excluding front padding.
  It is available with INFER alone, also to C++ and Zig, and transfers no ownership.

---

## MQTT clients

- C owns one reusable buffer pair per initialized client and each attempt's
  socket/TLS session separately. A heap pair is allocated lazily and
  atomically; disconnect or failure releases the transport, deinit/destroy also
  frees the pair. Zero-heap pairs are embedded. A `Busy` rejection preserves all
  live state; an accepted reconnect resets local clean-session subscriptions.
  CONNECT/RX preflight completes before any effect.
- C++ callbacks use an instance field with `user_data=this`; the client's
  stable address and non-movability keep the context valid. A failed connect
  restores the previous callback, and clients route independently. Rust stores
  a function pointer in the C user data; Zig uses a comptime trampoline or typed
  context. Owning cleanup in every binding releases an active transport.
- All operations on one client must be serialized by the caller. A synchronous
  callback may disconnect without losing its packet views, but must return
  before reconnect, destructive cleanup or another packet operation on that
  client. Views cannot escape the callback. Raw C destroy invalidates the
  handle; destroying through a stale handle is not supported.

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
| Audio node / tap | the thread driving the graph (audio thread, or the caller of `process`) | stop / deinit wait for a running cycle and stop the device sink's thread (never call them from a callback) |
| httpd handler / WebSocket | the single `httpd` thread, one at a time | stop waits for that thread, which closes open WebSocket connections and calls their `on_close`; a connection handle is valid until its `on_close` returns |
| Shell command | the thread that feeds `process_char` / `process_line` / `process_line_with_hook`, one command at a time under the shell's lock | — (never feed the shell from an ISR; a handler must not wait for another thread using the shell) |

**Contract notes**

- A binding that wraps a callback in a safe API must not promise more than this
  table does. Where a row names a gap, the wrapper documents it rather than
  hiding it (the shell docs in every binding say a handler must not wait for
  another thread using the shell).
- Strings a callback API keeps by pointer (shell command names and help, thread
  names on NuttX, Zephyr and the host backend) must outlive the registration:
  Rust takes `&'static CStr`, Zig comptime or caller-owned names, C++ documents
  the rule.
- No binding keeps a shell command table of its own, so registration is as
  thread-safe as the C shell: C++ passes the C descriptor through, Zig makes a
  trampoline per handler at comptime, and Rust takes a `Send + Sync`
  function item or closure that captures nothing and monomorphises a
  trampoline per handler (a function pointer value does not build).

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
| Canvas pixel backing | borrowed native buffer | copied `CanvasBuffer` or initialized `allocate_buffer`, native widget owns pixels | borrowed native buffer |
| Typed constants | `LV_ALIGN_*` / `LV_PALETTE_*` (C macros) | `lvgl::ALIGN_*` / `Palette` | `lvgl.ALIGN_*` / `lvgl.PALETTE_*` |
| Raw escape hatch | implicit `lv_obj_t*` / `handle()` | `Widget::raw()` + `bindings::` | `obj` field + `ove.ffi` |

**Contract notes**

- Rust Canvas views are `Copy` and pixel storage belongs to the native widget:
  `set_buffer` copies tightly packed rows, `allocate_buffer` creates zeroed
  pixels, both use LVGL's pool and return `Result` (`InvalidParam` for bad
  dimensions, format or length, `NoMemory` on allocation failure), and a failed
  replacement keeps the current pixels. The raw buffer and layer methods are
  `unsafe`. C++/Zig Canvas buffers are borrowed, initialized, native-stride
  backing that the caller keeps valid until native close.
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
| Unknown / unmapped code | Unnamed `Error` value (carries the code) | `Error::Unknown(i32)` (carries the code) | `Error.UnknownErrorCode` in release; **panics in `Debug`** |
| Struct-layout pin (`cmake/ove_abi_types.def`) | none (same compiler as C) | `build.rs` `const` asserts | `comptime` in `abi_contract.zig` |

**Contract notes**

- Static assertions in `include/ove/types.h` pin the numeric error codes.
  C++ defines `Error` variants using the C constants, Rust uses bindgen's
  constants in `from_code` and `to_code`, and Zig matches the imported C
  constants in `mapErrorCode`.
- Unknown-code policy: Rust always surfaces `Error::Unknown(code)`. Zig cannot
  carry a payload in an error set, so it **panics in `Debug`** (loud drift
  detection during development) and **degrades to `Error.UnknownErrorCode` in
  release**, so a shipped binary never crashes on a code the binding has not
  mapped yet.

---

## Version

The oveRTOS version (`include/ove/version.h`) is exposed as compile-time
constants in every binding; see [Versioning](versioning.md).

| Contract | C++ | Rust | Zig |
|---|---|---|---|
| Numbers | `ove::version::major` / `minor` / `patch` | `ove::version::MAJOR` / `MINOR` / `PATCH` | `ove.version.major` / `minor` / `patch` |
| Packed, for comparisons | `ove::version::packed`, `number()` | `ove::version::VERSION`, `number()` | `ove.version.packed_version`, `number()` |
| String | `ove::version::string` (`std::string_view`) | `ove::version::VERSION_STR` (`&str`) | `ove.version.string` (`[]const u8`) |
| Checked against its package manifest | — (no manifest) | at compile time: `Cargo.toml` must match the header | — (the firmware build does not read `build.zig.zon`) |

**Contract notes**

- All three take their values from the C macros (C++ directly, Rust through
  bindgen, Zig through `@cImport`), so none can disagree with the header. The
  exception is a docs.rs build of the Rust crate, which reads hand-kept copies
  in `bindings_stub.rs`. The `version-pin` lint (`make lint`) keeps those and
  the manifests (`Cargo.toml`, `build.zig.zon`, the CLI's `pyproject.toml`) in
  step with the header.

---

## Maintaining this contract

When you add or change a binding API, update this page in the same change so the
three languages stay aligned. State the rule, not the history of how it was
reached; that belongs in the commit message. The examples under
`apps/{cpp,rust,zig}/` act as conformance tests for these semantics — if an
example has to drop to raw FFI to express something, that is a gap in the
contract (or the wrapper), not a licence to diverge.
