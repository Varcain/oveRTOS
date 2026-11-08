# Binding Semantics

Relative synchronization and IPC waits share the native engine budget across C, C++, Rust and Zig. On NuttX, positive finite waits round up to ticks and saturate at min(WDOG_MAX_DELAY - 2, INT32_MAX - 1) ticks: 1,073,741,821 with 32-bit clocks or 2,147,483,646 with 64-bit clocks. Timeout and stream short counts refer to this effective budget. Only explicit FOREVER waits are indefinite; native tick phase, scheduling and condition-variable mutex reacquisition can extend the elapsed time.

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

- Rust privately terminates genuine bounded C-string inputs (FS255 bytes,
  TLS hostname255, method7/path63) and passes counted inputs to native
  NVS/response/log consumers. Byte slices must never be handed directly to a
  terminator-reading consumer. NUL is rejected before effects. FS byte paths
  preserve non-UTF8 directory names. Native `_cstr` compatibility remains.
- Checked formatting runs once, latches errors, and publishes no partial key
  or path. Core staging needs neither LVGL nor a Rust allocator.
  `FmtBuf::as_c_str()` remains an optional low-level compatibility utility.
- Audio retains static `name_span { data, len }` bytes without a terminator,
  name cap or name allocation. This changes the public descriptor ABI;
  rebuild all consumers and use length-aware readers. Shell/thread name
  retention remains unchanged.
- Rust LVGL string methods return `Result`: embedded NUL is `InvalidParam`,
  conversion pool exhaustion is `NoMemory`. Short conversions use stack
  scratch; longer ones use LVGL's fixed pool without a Rust global allocator.
  `set_text_fmt(format_args!(...))` and `cell_value_fmt` accept Rust formatting.
  They preserve text on preparation errors; LVGL's own void setters still
  follow its configured assertion policy for internal allocation failures.
- `Label::text_buffered` attaches a static `TextStorage<N>` once. Its
  `BufferedLabel` updates allocate no text memory; capacity includes NUL.
  Overflow returns `NoSpace` without truncating UTF-8 or changing the text.
  Replacement and deletion invalidate old tokens; storage can then be reused.
- `Label::bind_text` accepts `fn(i32, &mut dyn core::fmt::Write) -> fmt::Result`.
  The binding owns observer context until replacement, deletion, or subject
  teardown. `text_status()` reports asynchronous formatting errors and
  `NotFound` when unbound. Recursive state changes that invalidate a render
  report `Busy` unless LVGL dispatches a fresh render of the latest value.
- All LVGL operations, including conversion allocation and storage updates,
  require initialized LVGL and its existing lock. Callbacks already holding
  that lock must not reacquire it. Image strings are borrowed only until the
  widget copies them; image descriptors and pixels must remain alive.
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
| Current thread | `this_thread::self()` / `get_id()` (null handle on a thread oveRTOS did not create) | `Thread::current() -> Option<Thread>` (`None` there) | `getSelf()` (null there) |
| Priority | `ove_prio_t` (C enum) + `ove::prio_above`/`prio_below` | `Priority` newtype (`u8` level, anchor consts, `above`/`below`) | `Priority` non-exhaustive `enum(c_uint)` (`above`/`below`) |
| State | `ove_thread_state_t` (C enum) | `ThreadState` enum | `State` `enum(c_uint)` |

Native pthread workers return no oveRTOS identity even when their creator is an
oveRTOS task; they do not inherit its implicit stop flag or state attribution.

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
| Occupancy (snapshot) | `size()` / `size_from_isr()` / `capacity()` / `empty()` / `full()` | `len()` / `len_from_isr()` / `capacity()` / `is_empty()` / `is_full()` | `len()` / `lenFromIsr()` / `capacity()` / `isEmpty()` / `isFull()` | — |

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

- Rust's nonowning `MutexAccess` provides `Deref`/`DerefMut`, is `!Send`, and
  cannot escape the callback. Dropping or forgetting it leaves private release
  ownership intact. End the callback at an old guard's early-release point.
  RecursiveMutex has matching no-data callback methods and no public unlock.
- Rust callbacks return `ove::Result<R>`. For a distinct application error,
  map it explicitly or return `Ok(app_result)` and use `??` at the call site.
- Native contention/expiry maps to Rust errors (currently Timeout; native
  same-thread recursive entry to an ordinary data mutex is rejected as Busy).
  C++/Zig boolean try methods return false on contention.

---

## Synchronization construction and counts

The five native synchronization families publish a handle only after complete
initialization. Failure preserves the output and releases successfully
initialized pieces before backing can be retried or freed. Teardown requires a
successfully initialized, quiescent object with no waiters, active operations or
outstanding mutex acquisitions.

C++ constructors and void acquisition/wait methods invoke the configured
assertion on error and abort if it returns, including release builds. Boolean
and `Result` methods retain their fallible contracts. Rust constructors return
errors; Zig constructors propagate errors and release their allocated storage,
while its void acquisition methods use a terminal panic.

Semaphore counts require `max > 0`, `initial <= max` and the provider ceiling:
`SEM_VALUE_MAX` on POSIX/NuttX, `UBaseType_t` on FreeRTOS and `unsigned int` on
Zephyr. FreeRTOS and Zephyr retain and enforce the requested maximum. POSIX and
NuttX posts enforce their provider-wide ceiling; they do not retain the requested
`max`. This limitation applies equally to C, C++, Rust and Zig.

Bindings can query interrupt context with `CONFIG_OVE_SYNC` alone, without
ASYNC, TIME or object initialization. Critical-section lock/unlock still require
ASYNC. POSIX timer dispatchers bracket callbacks with the simulator context flag;
this does not establish general asynchronous signal-handler or IRQ exclusion.

Rust Mutex, RecursiveMutex, Semaphore, Event and CondVar use `ove_sync_context`
privately before resource operations, waits and native deadline queries.
RESOURCE permits legal FreeRTOS setup before scheduler start; BLOCKING requires
an active non-idle thread and enabled interrupts. Unsupported Result methods
return NotSupported; CondVar signal/broadcast have no effect there.
FreeRTOS admission supports single-core configurations; multicore FreeRTOS and
NuttX SMP return no context flags. Private
already-owned releases still run when a callback leaves interrupts masked.
Unavailable resource teardown retains native backing and its static claim,
while `T` is dropped or extracted once. Typed zero-heap storage rejects duplicate
live initialization as Busy, reopens after completed cleanup and stays reserved
when its owner is forgotten. All five creation macros use typed admission.
Recursive scopes admit at most 65,535 combined held/pending acquisitions.

PRODUCER admits task semaphore/Event posts and serialized notification setup
in recognized masked task contexts, including nested critical sections.
FreeRTOS also permits these operations before scheduler start and while its
scheduler is suspended. This preserves synchronous async wake tails while
RESOURCE and BLOCKING remain unavailable in masked running tasks. Producer
admission does not permit mutex acquisition or arbitrary nonblocking operations.
Rust semaphore releases outside producer admission have no permit or callback
effect, including in ISR context. Both Rust Event producer names select the
actual task or ISR route; native ISR setup must satisfy its kernel-aware
priority/grouping contract. The query does not classify arbitrary IRQ or NMI
eligibility. C, C++ and Zig retain their own native operation/context contracts.

Semaphore/Event zero-heap constructors claim their typed backing before native
initialization. Heap construction ignores the supplied typed storage. Legal
cleanup finishes before a claim reopens; forgotten or unsupported-context drops
retain backing and its claim. Unsafe raw backing must remain stable through
cleanup or indefinite retention. The native C storage layout is unchanged.

Rust `Semaphore::set_notify` is unsafe: registration, replacement and removal
must be serialized with all native gives and other registrations, and the old
callback/data must remain live until all in-flight calls finish. The context
query supplies no registration synchronization. The callback and its data must
support concurrent or reentrant calls from all release callers, or those gives
must also be serialized. Current native give paths call
the hook after a post attempt even when their count ceiling prevents increment.
Rust zero-time semaphore/Event waits preserve the provider's empty/timeout
status; unsupported context instead returns NotSupported before native entry.

Rust `AsyncSemaphore::arm` takes exclusive static access and returns a shared
static reference only after installing its wake callback. Initialize the wrapper
at its final address, arm it once, then publish that returned reference to
producers and async waiters. Shared access cannot register again or overlap
setup with an `inner()` producer borrow. An error still consumes the static
borrow and retains the wrapper/backing; safe code cannot retry or reclaim it.
This Rust setup rule changes no native, C++ or Zig interface. It does not
establish fairness for multiple waiters sharing the single waker slot.

The native `ove_condvar_wait_checked` entry reports whether the calling thread
owns its ordinary mutex on every return, including errors. Success and clean
timeout report held; other errors can report held or released. The output must
be writable and disjoint from the primitive backing. Rust consumes its current
view and uses this output to preserve private cleanup on either error outcome;
an error returns no view. Success/clean timeout return reacquired access. C++
and Zig convenience methods use the legacy status contract.
Both entries finish waiter retirement before returning; cleanup of a permanently
broken internal mutex has no bounded-recovery guarantee.

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

## Inference

The backend enforces one admitted session across handles: init/create returns
`OVE_ERR_BUSY` before storage/output writes, allocation or interpreter setup.
Admission covers constructor failure cleanup, destructors and final frees.
Losing a session without closing it retains admission indefinitely; a new
constructor does not recover it. Raw operations on one live handle must be
externally serialized, and retained model bytes/storage/arena must outlive the
final close call returning. An old handle cannot be reused after its storage
is initialized for another session. This rule does not validate arbitrary
model graphs or make arbitrary arena sizes safe; existing valid-model and
sufficient-arena preconditions remain.


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

Successful input/output descriptors preserve the exact float32, int8, uint8,
int16 or int32 type and rank zero (scalar) through five. Unused dimensions are
zero. The C getters leave the caller's descriptor unchanged on error. C++ typed
pointer helpers return `nullptr` on descriptor errors; Rust and Zig getters
propagate the native error. Output data is read-only.

These bindings share the [trusted-model and arena contract](inference-runtime.md).
Models are immutable build-time assets aligned to 16 bytes; there is no general
model validator. Below the startup minimum, setup returns NoMemory (OutOfMemory in Zig) before SDK
construction. Once persistent/preparation needs are met, exhausted allocation
or planning capacity also returns NoMemory (OutOfMemory in Zig). Intermediate undersized configurations
have undefined behavior. Each admitted setup clears the effective arena in
O(arena_size) time. Outputs are read-only.

Measure the configured target/provider and retain at least reported usage plus
16 bytes while keeping its qualified preparation capacity. Final usage can
exclude temporary setup peaks; shrinking until failure is unsafe. Re-measure
after model/provider/TFLM changes. In C the query is `ove_model_arena_used()`;
null or accessible closed handles return zero. A dangling handle is invalid.

C++ embedded zero-heap `Model<N>` rejects a configured arena larger than N
before native initialization and forwards accepted capacities unchanged.
It keeps the constructor's assertion policy; if the hook returns or assertions
are disabled, failure leaves an empty model with a null handle. Heap models
allocate the requested capacity independently of N. Cleanup runs before
embedded storage members are destroyed. `ModelStorage<N>::load` uses exactly
its own arena capacity and returns errors through `Result`.

Zig uses `ove.infer.linkedModel("g_my_model_data")` for generated array and
length symbols. The binding references the array address and loads the scalar
length separately. The integration establishes a complete immutable initialized
array, matching `unsigned int` length, actual 16-byte alignment and program
lifetime. This helper supplies metadata and does not validate arbitrary linkage
or native model programs. `modelSlice` remains a low-level truthful-pointer/
length adapter.

Zig zeroheap `init` rejects a configured size larger than the supplied slice
before native effects and preserves accepted sizes exactly. It publishes
tracking state only after native success; failed live attempts, including Busy,
preserve the existing owner. A failed first init grants no usable object or
cleanup rights. Retain model bytes, arena and stable wrapper storage through
close and serialize access/reuse. Repeated close on an initialized closed object
is a no-op, including Debug; live pin validation remains active. Heap creation
keeps native allocation behavior.

Rust creates a private-field `ModelConfig` with unsafe
`ModelConfig::from_trusted_model(bytes, capacity)`. This declares qualification
of the native program, target/provider build, exact configured capacity and
setup/teardown behavior; it does not validate arbitrary model programs. The
token retains the immutable bytes and cannot safely be changed to a smaller
capacity. `Model::new(&cfg)` and `ModelStorage::load(&cfg)` retain the asset
loan independently of the temporary config object. A borrowed session also
exclusively borrows its storage. `ModelStorage::new` is const and its arena
field is individually aligned to 16 bytes. A larger storage buffer still uses
exactly the qualified configured size; a smaller buffer is rejected before
native effects. Native allocation needs no Rust allocator or `alloc` feature.

Rust tensor views accept only sealed `f32`, `i8`, `u8`, `i16`, and `i32`
elements and require the exact descriptor type. They check whole elements,
alignment, representable lengths and the complete retained allocation before
forming references. Mutable input views require `&mut Model` and lie wholly
inside its effective writable arena. Shared output views can also borrow
immutable model-backed constants. Empty views are valid independent empty
slices. The view's borrow excludes incompatible views, invocation, reload and
close. Native setup zeroes the entire effective arena; defined invocation
must preserve valid initialized backing even on an ordinary error. Reading
initialized stale outputs does not require a successful prior invocation.

`unsafe Model::invoke(&mut self)` separately requires a qualified numeric
input/state domain for the exact program/build. An error return does not
excuse an undefined call. The original live keyword examples are build
coverage until their live microphone domains are qualified; a finite
reference corpus does not establish that broader domain. The generated
`model_data!` accessor is also unsafe: the integration establishes its linked
array/length pair, full initialized immutable span and program lifetime.
Applications use the binding's config/storage/view APIs without application FFI.

Rust keeps `Send` and shared-reader `Sync`: moving an owner leaves backing in
place, shared native metadata queries read eagerly initialized records, and
all mutation needs exclusive access. Construction and invocation reject
detected ISR context with `InvalidParam`. Normal Drop synchronously calls the
matching native deinit/destroy once. ISR Drop performs no native handle access,
teardown or free, retaining resources and admission permanently. This uses the
same no-worker/no-old-owner-access policy as forgetting a borrowed session;
later construction returns Busy before touching abandoned backing. Host POSIX
reports no hardware ISR, independently of simulated ordinary-thread callbacks.

The C binding-support queries `ove_model_binding_arena` (live effective span,
excluding front padding) and pointer-free `ove_model_binding_in_isr` are
available with INFER independently of ASYNC, including through C++ and Zig's
C import. They transfer no ownership. Raw descriptor pointer access and raw
`Model::from_static` integration still require their documented unsafe backing
and serialization contracts.

**Contract notes**

- The arena is the bulk of a session's memory, so it lives in caller storage
  meant to be static, never inside an object that may land on a thread's
  stack. A zero-heap C++ `ove::Model<N>` with `N > 0` still embeds its arena,
  for compatibility; prefer `ModelStorage`.
- One model session at a time: the backend shares one interpreter between
  sessions, so a pipeline loads its models into one storage in turn, ending
  each session before loading the next.
- Asymmetry: Zig has no heap-mode form that borrows caller storage; its heap
  build allocates per session with `Model.create`.

---

## MQTT clients

C owns one reusable buffer pair for an initialized client and each attempt's
socket/TLS session separately. Heap allocation of the pair is lazy and atomic;
disconnect/failure releases transport, while deinit/destroy also frees the pair.
Zero-heap pairs remain embedded. BUSY rejection preserves all live state; accepted
reconnect resets local clean-session subscriptions. Complete CONNECT/RX preflight
must precede effects. C storage ABI stays unchanged by this ownership policy.

C++ callbacks use an instance field with `user_data=this`; its stable address
and existing non-movability keep the context valid. Failed connect restores the
previous callback, and independent clients route independently. Rust stores a
function pointer in C user data; Zig uses its static/comptime trampoline or typed
context. Owning cleanup in each binding releases an active transport through C.

All operations on one client require caller serialization. A synchronous callback
may disconnect without losing its packet views; it must return before reconnect,
destructive cleanup or another packet operation on the same client. Views cannot
escape callback return. Raw C destroy invalidates its handle; this does not
promise redundant destruction through a stale handle or extend every wrapper's
existing idempotence contract.

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
| Protected task | the caller's thread, synchronously | a contained fault abandons it by longjmp |

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

- Rust Canvas views remain pointer-sized and `Copy`. Pixel storage belongs to
  the native Canvas subclass and survives dropped or forgotten Rust views. It
  remains live through object and child deletion callbacks, then native
  destruction releases it. `set_buffer` copies an immutable initializer before
  returning; `allocate_buffer` creates zeroed pixels, with a black/white I1
  palette. Both return `Result`, use LVGL's existing pool in heap and zero-heap
  builds, and need no Rust allocator. Replacement temporarily needs both old
  and new allocations; preparation failure preserves the current pixels.
- Rust Canvas initializers contain tightly packed rows. I1 has two BGRA palette
  entries (eight bytes), followed by each row rounded up to whole bytes. The
  binding handles native row padding and alignment. Nonpositive dimensions,
  unsupported formats, length mismatch and native header/stride overflow return
  `InvalidParam`; allocation failure returns `NoMemory`. Native widget creation
  keeps upstream LVGL's allocation/assertion behavior. C++/Zig raw Canvas buffer
  APIs borrow writable initialized backing with the configured native stride,
  palette and alignment; their callers keep it valid through native close.
- Rust's raw Canvas buffer and layer operations are unsafe escape hatches.
  Layer storage, queued tasks and referenced drawing resources must remain live
  and exclusive through paired init/finish, with unchanged canvas backing. The
  binding evicts normalized image caches after ordinary pixel/fill mutations
  and layer completion. Existing widget-handle, locking and native callback
  constraints still apply; this ownership rule does not enforce all LVGL
  lifecycle or thread-safety contracts in the type system.
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

I2C's native boundary validates seven-bit addresses and independent TX/RX byte
counts before locking, hardware or async publication. STM32 accepts at most
65535 bytes per phase; Zephyr counts fit uint32_t; NuttX counts fit ssize_t and
the supported F7 int counter. No automatic splitting may change STOP/repeated
start behavior. Register writes count the prefix byte. Empty TX in write-read
means plain read; zero-byte synchronous writes/probes preserve the address phase.
NuttX's two-message combined operation sets NOSTOP first and READ last. These
native boundary checks do not establish Rust async retained-buffer ownership.

Synchronous Rust I2C uses ordinary Config, native-heap owners and claimed
static I2cStorage. Bus methods require exclusive access; owners are not
Copy/Clone/Send/Sync. Static claims reopen only after native cleanup returns,
while forgotten owners keep their claim. Unsafe raw wrappers remain nonowning.
The embedded-hal adapter validates the complete operation list before effects:
it supports [], one write (including empty), one nonempty read, and nonempty
write followed by nonempty read. Other shapes, including an empty write prefix
before read, fail without dispatch. Inherent empty-prefix write_read remains a
plain read. Relative I2C/SYNC durations do not require TIME; clock-based SYNC
methods do. With CONSOLE absent, the existing Rust log output is a no-op.
Synchronous owners require raw async users to retire before cleanup.

Asynchronous Rust I2C uses the same Config with AsyncI2c::new in heap builds or
claimed AsyncI2cStorage<TX, RX=TX> in either allocation mode. Stable native
backing and independent TX/RX byte staging need no application FFI, Rust global
allocator or manual pinning. Operations require &mut self. Owners and operation
futures are neither Send nor Sync; owners are neither Copy nor Clone. Moving an
owner keeps native backing in place. Either static capacity may be zero;
exceeding either returns NoSpace before I/O. Heap growth preserves each phase's
high-water capacity, checks the combined layout and allocates the new pair
before freeing the old pair, with native alignment overhead.

Dropping an operation detaches its waiter; forgetting it does not run Drop, so
the waiter remains until completion, replacement or owner close. Both release
application borrows without cancelling I/O: native code retains internal spans,
and only a successful live future copies RX back. Active orphans return
BusBusy for new work. A new operation can replace a ready orphan; flush awaits
and consumes its actual result without RX copyback. Owner Drop defers cleanup
through native callback, wake and submission tails; static claims reopen only
after native deinit returns. Missing completion or a forgotten owner retains
one bounded context/capacity or claim. There is no watchdog, filler or split.
Separate handles still require physical-bus coordination.

The embedded-hal-async seven-bit adapter preflights the entire list: [] is a
no-op (address still checked; orphan not flushed), a nonempty write/read or
nonempty write followed by nonempty read uses one native transaction. Other
lists return NotSupported before effects. Empty single phases and a nonempty
write with empty read return InvalidParam; an empty write prefix before read,
including both-empty, returns NotSupported. Trait write_read follows that
policy; inherent write_read uses empty TX as plain read, empty RX as pure write,
and rejects both empty. Independent counts and provider limits apply unchanged.
The minimal closure is native I2C, ASYNC, SYNC, TIMER and TIME plus Rust async;
SPI/GPIO/CONSOLE are unnecessary. NuttX debug zero-heap preparation returns
NotSupported; release zero-heap retains the native worker-reserve behavior,
whose reclamation can outlast notification. Static Rust staging does not alter
that native resource policy.

SPI's native boundary counts bytes in either word mode. In 16-bit mode it
requires whole frames and `uint16_t`-aligned buffers. Raw TX/RX spans cover the
full count and may be disjoint or exactly identical in-place; partial overlap
is rejected. All nonempty sequence records and backend limits are checked
before effects. Software CS and the mutex for one ove handle span a synchronous sequence;
async use needs separate coordination because it does not take that mutex;
native hardware selection/controller ownership may end between segments on
NuttX and Zephyr. Read filler is backend-defined, possibly initial RX contents.
STM32 synchronous chunks share one per-segment timeout budget; its one-shot
async limit is 65535 frames.

Synchronous Rust SPI owners select their native word size: `Spi` uses `u8` and
`Spi16` uses `u16`. Public `Config`, `ChipSelect` and borrow-retaining `Transfer`
descriptions support ordinary calls without application FFI. Unequal slices
become a full-duplex prefix and a half-duplex tail in one native sequence;
neither buffer is published beyond its supplied span. Empty operations are
no-ops. Every logical descriptor occupies two native records, with checked
flattened count and no temporary array proportional to the sequence length.
Heap owners use native create/destroy; static `SpiStorage::init` claims stable
backing until native deinit finishes. Forgetting a static owner retains its
claim; unsafe raw wrappers are nonowning. Owners are not Copy/Clone/Send/Sync.
Native backend limits and coordination with separate handles remain the
caller's responsibility. Zig slice adaptation remains separate.

Rust async SPI uses typed `AsyncSpi`/`AsyncSpi16` owners and
`AsyncSpiStorage<N, W = u8>` with ordinary `Config`/`ChipSelect`. It stages the
complete larger slice length, zero-pads missing TX and discards excess RX in
one native operation. Successful live futures copy only requested RX; error
completion leaves application RX unchanged. Static capacity counts words and
returns `NoSpace`; heap growth uses the native allocator and can fail.

Native code retains only stable internal backing and copied CS. Future Drop
or forget does not cancel I/O. Active orphans reject new nonempty work with
`BusBusy`; empty work does nothing, and flush awaits/consumes the actual result.
Owner Drop marks closing; independent native/callback/wake/submission lifetimes
defer deinit/free/static-claim release until their final access. No future-owned
reference count depends on Drop after Pending. Metadata locking never encloses
Waker code, native calls, allocation or payload copying. Owners and futures
remain neither Send nor Sync. Forgotten owners or missing completions retain
bounded backing/claims. The old raw-handle constructor is removed.

The async feature itself requires no Rust global allocator. The executor uses
its static slot when `alloc` is disabled; with `alloc` in heap mode it keeps
its existing Box allocation. SPI heap backing always uses native allocation.
Native frame limits, task-context/nonblocking callback obligations, physical
bus coordination and NuttX worker reserve/debug-zero restrictions still apply.

When you add or change a binding API, update this page in the same change so the
three languages stay aligned. The examples under `apps/{cpp,rust,zig}/` act as
conformance tests for these semantics — if an example has to drop to raw FFI to
express something, that is a gap in the contract (or the wrapper), not a licence
to diverge.

Native Queue initialization validates nonzero dimensions, product overflow and
provider limits before entering the kernel. Partial initialization failures
retire earlier resources and do not publish an output handle, permitting retry
on the same quiescent backing. Rust and Zig propagate the error; C++ applies
its constructor initialization assertion after native cleanup. C callers must
keep storage and the complete item buffer exclusive through teardown. Queue-only
configurations provide interrupt detection and resource-context queries without
enabling SYNC, TIME or ASYNC; this does not broaden the admitted context flags.

Rust Queue holds one claim on typed zero-heap backing until complete native
cleanup. Live, initializing, tearing-down, forgotten and retained owners reject
another claim with Busy before native entry. Clean initialization errors reopen
the claim; Raw owners still deinitialize their exclusive backing. Heap owners
allocate independently. Queue payloads are invariant: shared insertion cannot
shorten a stored reference's lifetime. Borrowed heap payloads remain supported,
and owner Send/Sync requires Copy + Send without requiring T: Sync. Private
unused typed storage remains shareable for local non-Send Copy payloads.

Rust construction and destruction require RESOURCE; unavailable Drop retains
the resource and any claim. Task transfers, including zero timeout, require
BLOCKING before native or deadline-clock effects. ISR-named methods dispatch
actual task callers through the admitted task route. Native interrupt priority
and setup requirements still apply. Relative durations work without TIME;
absolute deadlines require TIME. Infallible task occupancy snapshots return
inert zero outside admitted context, so is_empty returns true and is_full false;
this fallback does not report actual emptiness. Capacity is validated constant N.
Raw backing and callback installation retain their documented unsafe contracts;
notification setup requires RESOURCE and serialization through all callback
tails. These Rust ownership rules do not impose a new C++ or Zig lifetime API.

Rust AsyncQueue setup consumes exclusive static access and returns shared ready
access only after callback installation. Producers and receive futures use that
shared reference. Safe permanent placement can use StaticCell without a core
allocator dependency or application FFI. Setup uses Queue's RESOURCE admission;
producer-only masked contexts are insufficient. Error consumes the static borrow
too, retaining wrapper/backing and preventing safe retry or reclamation. Static
lifetime keeps callback data alive after publication, while exclusive setup
prevents registration from overlapping another registration or producer.

EventGroup native initialization publishes a handle only after all native stages
succeed. Failures release earlier stages and preserve the initialization error;
heap helpers release failed allocations. This contract applies to C initialization
and the C++/Rust/Zig constructors. The existing pointer-free context query is
available with EVENTGROUP alone, including without SYNC, QUEUE, TIME or ASYNC.
EventGroup Rust storage ownership and operation admission are separate repairs.

EventGroup masks use the configured payload domain exposed by the ordinary C,
C++, Rust and Zig accessors. FreeRTOS reserves its native high byte; no global
24-bit restriction applies to the other engines. Invalid scalar masks return
zero without effects; ISR clear and invalid waits report InvalidParam. Waits
reject zero masks and unknown flags without changing the result or reading a
deadline clock. Zero set/clear masks remain valid. Task clear returns the full
prior value. NuttX snapshots share writer exclusion; Zephyr clear shares the
native event lock through a private syscall that preserves granted native user
callers. Zig timed waits expose InvalidParam alongside Timeout; its scalar
forever wait requires valid inputs and panics on an unexpected error. Larger
set/wait result normalization remains separate work.

FreeRTOS EventGroup deferred commands use stable internal tokens. Teardown
cancels queued associations before native deletion and immediate backing release;
external sources/producers and synchronous users must already be quiescent.
The daemon protects its native update against task teardown, detaches owner/token
access, then invokes saved notify locals after publication. Clear propagates the
daemon wake request too. Native MPU V1 task teardown preserves caller privilege.
These guarantees keep C++ inline and Zig allocator-owned retirement unchanged.
Notify data is separate: code/data must remain live through pending dispatch and
saved tails, and unregister/deinit is not a data reclamation fence.

Rust EventGroup typed storage claims one native generation until complete legal
cleanup. Live, initializing, tearing-down, forgotten and retained generations
exclude another zero-heap owner. Heap constructors remain independent; raw
constructors require stable exclusive backing through actual cleanup or retention.
Construction, cleanup and notify setup require RESOURCE permission. Unsupported
Drop retains the native resource and claim. Task operations, including zero waits,
require BLOCKING; unsupported scalar calls return inert zero and Result methods
return NotSupported before primitive or clock effects. ISR-named task calls use
task operations and their snapshots; task clear preserves InvalidParam. Genuine
interrupt calls retain native eligibility and deferred-result contracts.
Relative Duration waits work without TIME; only absolute Instant methods require
TIME. The typed eventgroup macro preserves this ownership policy automatically.
AsyncEventGroup exclusive setup remains a separate repair; native cancellation
and Rust ownership do not certify its shared registration method.
