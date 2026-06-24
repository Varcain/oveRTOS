# Plan: real pthreads (LinuxThreads) on the NOMMU Linux personality

## Context

uClibc-ng provides NOMMU pthreads via **LinuxThreads** (the overtos build sets
`UCLIBC_HAS_THREADS=y` / `UCLIBC_HAS_LINUXTHREADS=y`; NPTL is off — it needs MMU/TLS the
NOMMU target lacks). But the personality cannot host them today:

- `backends/common/ove_lnx_run.c:276` handles **every** `clone()/fork()/vfork()` as a
  **vfork**: capture the parent ctx, suspend the parent (no live thread), spawn the child
  sharing the parent's region; the parent resumes only when the child **execs** (into its
  own region) or **exits**.
- A pthread thread is `clone(CLONE_VM)` that **never execs** — it runs a function in the
  shared memory. Under the vfork model the parent would stay suspended until the thread
  exits → no concurrency. And `futex(2)`/`futex_time64` is a benign single-threaded stub
  (`linux/ove_linux_syscall.c`: WAIT→-EAGAIN, WAKE→0).

So there are no concurrent threads for a futex to coordinate. BusyBox is single-threaded,
so nothing hits this yet — but any real multi-threaded program needs the work below.

**Good news for NOMMU:** a thread *shares* the address space (`CLONE_VM`), so — unlike
`fork()`, which needs a memory copy NOMMU can't COW — threads need **no new region**. They
are actually *easier* than fork on NOMMU. The two missing pieces are (1) co-running the
thread without suspending the parent, and (2) a real futex wait/wake.

## Design

### Phase T1 — Thread clone: co-running, shared region

Discriminate a thread from a fork/vfork by the clone flags (ARM `clone`: r0=flags,
r1=child_stack, r2=parent_tid, r3=child_tid, r4=tls):

- **`(flags & CLONE_VM) && !(flags & CLONE_VFORK)` → THREAD.** Everything else keeps the
  current vfork/fork path (incl. `posix_spawn`'s `CLONE_VM|CLONE_VFORK`).

Thread spawn (a new path in `ove_lnx_dispatch` + the coordinator, parallel to fork but
**not** suspending the parent):
- Allocate a new **proc slot** (NOT a new region): `region` / `region_owner` = the
  parent's region; new `is_thread=1`, `tgid` = the parent's tgid (the group leader's pid),
  fresh `pid` (= tid).
- Child SP = `child_stack` (r1), a caller-allocated stack inside the shared region.
- Child resumes at the **clone-return PC** with r0=0 on `child_stack`; the **parent
  continues immediately** with r0=tid. Both co-run (reuse the Phase-D resume machinery,
  but resume *both* at once instead of parking the parent).
- Sizing: N threads share 1 region but consume N slots → `OVE_LNX_NSLOT` must comfortably
  exceed `OVE_LNX_NREG` (today NREG+4; bump for many-threaded programs). Thread stacks live
  in the shared region's arena/heap (caller-malloc'd) → multi-threaded programs may need a
  larger region or arena.

### Phase T2 — Real futex (`futex` / `futex_time64`)

Replace the stub with a real wait/wake queue keyed on the **physical** `uaddr` (in the
shared NOMMU region, `uaddr` is already a direct pointer all threads in the group see):

- A small global table of waiters: `{ uintptr_t uaddr; int slot; uint32_t bitset; }`.
- **FUTEX_WAIT / WAIT_BITSET (op 0 / 9):** atomically check `*uaddr == val`; if equal,
  enqueue this proc + **park** (new `futex_wait` per-proc flag, like `pipe_wait`/sleep, with
  an optional deadline from the timeout arg → `-ETIMEDOUT`); else return `-EAGAIN`. Woken →
  return 0; signal while parked → `-EINTR`.
- **FUTEX_WAKE / WAKE_BITSET (op 1 / 10):** dequeue + wake up to `val` waiters matching
  `uaddr` (and bitset); return the count woken (delivered at the waker's syscall boundary,
  the coordinator resumes the parked waiters).
- **FUTEX_REQUEUE / CMP_REQUEUE / WAKE_OP / PI ops:** start with `WAKE`-like / `-ENOSYS`
  and add as uClibc actually exercises them (condvars use REQUEUE; PI mutexes use LOCK_PI).
- Reuse the existing park/coordinator wake path (`event_post`/`event_wait`, the EV_PIPE
  pattern) — a `futex_wait`-parked proc is the same shape as a pipe-blocked one.

### Phase T3 — Thread exit + thread-group semantics

- **`exit` (1)** terminates only the **calling thread** (free its slot; if it was the last
  thread in the `tgid`, free the region). **`exit_group` (248)** terminates **all** threads
  in the `tgid`. Region freed on the last thread's exit (a per-region/per-tgid live-thread
  refcount).
- **`gettid` (224)** → the thread's pid (tid); **`getpid` (20)** → the `tgid` (leader pid).
- `ps`: threads share a `tgid` (Linux `ps` hides non-leader threads by default; `ps -T`
  shows them) — minimal first cut can just list them.
- Signals: a signal to the process (tgid) delivers to one/all threads. Minimal: deliver to
  the leader (or any live thread); full per-thread signal masks + thread cancellation
  (`pthread_cancel` via a signal) are a follow-up.

### Phase T4 — TLS (investigate, likely needed)

How does uClibc-ng LinuxThreads find the per-thread descriptor on ARM NOMMU?
- **Stack-based** (`thread_self()` derives the descriptor from SP) → nothing to do.
- **`__ARM_NR_set_tls` (0xf0005) → TPIDRURO** → handle `set_tls`: store a per-thread TLS
  pointer and have the engine seam load TPIDRURO when it switches to that thread (each Linux
  thread is already a distinct RTOS thread, so the RTOS context switch can carry it, or the
  personality sets it on resume).

Determine which by reading uClibc-ng's `libpthread/linuxthreads` for ARM + watching for a
`set_tls` trap at `pthread_create`. Plan for `set_tls` (cheap to add) regardless.

## Risks / NOMMU specifics

- **Manager thread:** classic LinuxThreads spawns a manager thread (extra `clone` + signal
  plumbing for create/join). uClibc-ng's reduced linuxthreads may or may not — verify early
  (the first `pthread_create` reveals the clone pattern + whether a manager appears).
- **Concurrency:** threads genuinely share memory + co-run. Single-core M-profile serializes
  svc dispatches (NVIC), so *kernel* state stays race-free at the syscall boundary; the
  *program's* own data races are real (that is real threading — the program's responsibility).
- **Robust futexes / `set_robust_list`** (already a no-op): on thread death the kernel
  normally walks the robust list to wake futexes — a follow-up once basic futex works.
- This interacts with the Phase-D coordinator (shared region + co-run + park/wake) — stage on
  its own branch with heavy 3-engine testing, like Phase D.

## Files

- `linux/ove_linux_syscall.c`: clone thread-vs-fork discrimination; real `futex`/
  `futex_time64`; `exit` (thread) vs `exit_group` (group); `gettid`/`getpid` tid/tgid;
  `set_tls` if used.
- `backends/common/ove_lnx_run.{c,h}`: thread spawn (shared region, co-run, no parent
  suspend); the futex wait queue + wake; per-region/tgid live-thread refcount for region
  free; new per-proc fields (`is_thread`, `tgid`, `futex_wait`, `futex_uaddr`).
- `include/ove/linux/syscall.h`: `is_thread`/`tgid`/`futex_*` proc fields; `CLONE_VM` /
  `CLONE_VFORK` / `CLONE_THREAD` flags; `FUTEX_*` op constants.
- `backends/{zephyr,freertos,nuttx}/*`: per-thread TLS (TPIDRURO) on thread resume if
  `set_tls` is used; spawn a co-running thread sharing the parent region.
- **Test program:** a small pthread test bFLT (BusyBox is single-threaded) — e.g. a
  Buildroot package or a hand-built `pthread_create` + mutex/condvar binary added to the
  rootfs. (This is also the first thing to build, to observe the real clone/TLS pattern.)

## Verification (all 3 engines)

- **T1+T2 core:** a 2-thread program incrementing a shared counter under a pthread mutex
  (futex-backed) N times each → final counter == 2N (no lost updates ⇒ the mutex blocks/wakes
  correctly). Both threads show live %CPU in `top` concurrently.
- **Condvar:** a producer/consumer using `pthread_cond_wait`/`signal` (futex REQUEUE/WAKE).
- **T3:** `exit_group` from one thread tears the whole process down (no stranded threads,
  region reclaimed); `pthread_join` returns the thread's status.

## Phasing

T1 (co-run thread) → T2 (futex) — the core, testable together with a minimal pthread
program — → T3 (exit/group/join) → T4 (TLS, fold in as soon as the first `pthread_create`
shows whether it's needed). Build the pthread test binary FIRST to observe uClibc-ng's
actual clone flags, TLS mechanism, and manager-thread behaviour before committing the design.
