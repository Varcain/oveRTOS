/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/thread.h"
#include "ove/storage.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"
#include "ove_backend_common.h"
#include "ove_thread_registry.h"
#include "ove_nuttx_priority.h"
#include "ove_nuttx_runtime.h"
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/tls.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>
#ifdef CONFIG_OVE_THREAD_STATE_STATS
#include <nuttx/note/note_driver.h>
#endif
#include <sched.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <malloc.h>
#ifdef __NuttX__
/* Only the firmware build knows the NuttX syslog; the host builds of the
 * observation code (tests/obs/nuttx_note) do not compile the diagnostics
 * below. */
#include <syslog.h>
#endif

/* A suspend sent before task_wrapper() installs the SIGUSR1 action relies on
 * the kernel dropping the signal (sched/signal/sig_dispatch.c: "There is no
 * default action"); with the POSIX default action the task would be killed
 * instead. */
#ifdef CONFIG_SIG_SIGUSR1_ACTION
#error "ove_thread_suspend() needs SIGUSR1 without a default action: disable CONFIG_SIG_SIGUSR1_ACTION"
#endif

/* Thread state has two roles on NuttX:
 *
 *  - t->state is functional: it reports whether the thread is parked in the
 *    SIGUSR1 suspend loop (SUSPENDED) and is the acknowledgement the
 *    suspending thread waits for.  The parked thread leaves its wait on the
 *    suspend semaphore, not on the state (see the handshake below).
 *  - With CONFIG_OVE_THREAD_STATE_STATS, the observed state (t->st, and the
 *    trace stream) follows the scheduler through the note driver below;
 *    the thread itself only marks RUNNING on entry, SUSPENDED/READY around
 *    a suspension, and TERMINATED on exit.
 *
 * Atomic store-release on t->state matches the destroying / enumerating
 * thread's atomic acquire-load — same race that TSan flagged in POSIX
 * (sanitizers can't reach NuttX bare-metal code, so this fix is by
 * cross-backend audit rather than a triggered TSan finding). */
#ifdef CONFIG_OVE_THREAD_STATE_STATS
static void obs_transition(struct ove_thread *t, int s);
static void obs_register(struct ove_thread *t);
static void obs_unregister(struct ove_thread *t);
#define OBS_TRANSITION(t, s)                                     \
	do {                                                     \
		irqstate_t obs_flags = enter_critical_section(); \
		obs_transition((t), (s));                        \
		leave_critical_section(obs_flags);               \
	} while (0)
#else
#define OBS_TRANSITION(t, s) ((void)0)
#endif

#define SET_STATE(t, s)                                               \
	do {                                                          \
		OBS_TRANSITION((t), (s));                             \
		__atomic_store_n(&(t)->state, (s), __ATOMIC_RELEASE); \
	} while (0)

/* Per-task pointer via NuttX task TLS.
 *
 * task_tls_alloc() hands out a *different* index on every call, so the
 * index has to be allocated exactly once: an unsynchronised check-then-act
 * can give two tasks two different indices, and the loser of that race
 * stores its pointer under an index nobody reads any more - its
 * tls_get_current() stays NULL forever.  Since the suspend handler
 * resolves the thread through the TLS, such a thread's suspend would
 * silently do nothing.  The allocation is therefore
 * done once, under a mutex; the fast path is a single atomic load. */
static int tls_index = -1;
static mutex_t tls_index_lock = NXMUTEX_INITIALIZER;

static void tls_ensure_init(void)
{
	if (__atomic_load_n(&tls_index, __ATOMIC_ACQUIRE) >= 0) {
		return;
	}
	nxmutex_lock(&tls_index_lock);
	if (tls_index < 0) {
		__atomic_store_n(&tls_index, task_tls_alloc(NULL), __ATOMIC_RELEASE);
	}
	nxmutex_unlock(&tls_index_lock);
}

static struct ove_thread *tls_get_current(void)
{
	int index = __atomic_load_n(&tls_index, __ATOMIC_ACQUIRE);

	if (index < 0) {
		return NULL;
	}
	return (struct ove_thread *)(uintptr_t)task_tls_get_value(index);
}

static void tls_set_current(struct ove_thread *t)
{
	int index;

	tls_ensure_init();
	index = __atomic_load_n(&tls_index, __ATOMIC_ACQUIRE);
	if (index < 0) {
		return;
	}
	task_tls_set_value(index, (uintptr_t)t);
}

/* ── Thread registry (intrusive list) ─────────────────────────────────
 * The shared registry (backends/common/ove_thread_registry.h), used for
 * trace descriptor enumeration and by the profiler for target enumeration.
 * Lock is a lightweight nxmutex (kernel) so it can be taken from any task
 * context. */
struct ove_thread *ove_backend_thread_registry_head;
static mutex_t thread_list_lock = NXMUTEX_INITIALIZER;

void ove_backend_thread_registry_lock(void)
{
	nxmutex_lock(&thread_list_lock);
}

void ove_backend_thread_registry_unlock(void)
{
	nxmutex_unlock(&thread_list_lock);
}

/* Non-blocking variant for the profiler's high-priority sampling worker,
 * which must never wait behind a lower-priority thread holding the lock. */
bool ove_nuttx_thread_registry_trylock(void)
{
	return nxmutex_trylock(&thread_list_lock) == OK;
}

struct ove_thread *ove_nuttx_current_thread(void)
{
	return tls_get_current();
}

static void _register_thread(struct ove_thread *t)
{
	nxmutex_lock(&thread_list_lock);
	ove_thread_registry_add(t);
	nxmutex_unlock(&thread_list_lock);
}

static void _unregister_thread(struct ove_thread *t)
{
	nxmutex_lock(&thread_list_lock);
	ove_thread_registry_remove(t);
	nxmutex_unlock(&thread_list_lock);
}

/* Store first thread for join in start_scheduler */
static struct ove_thread *first_thread;

/* Lane generation source for every thread start (see ove_backend_common.h). */
static uint32_t g_thread_generation;

/* ── Suspend/resume handshake ─────────────────────────────────────────
 *
 * NuttX has no pthread_suspend(), so a suspend is implemented with a
 * signal: ove_thread_suspend() sends SIGUSR1 to the target, the target's
 * handler parks it on t->suspend_sem, and ove_thread_resume() wakes it up
 * again.
 *
 * The signal alone is not a handshake.  NuttX queues the action and runs
 * it at the target's *next* dispatch
 * (arch/arm/src/armv7-m/arm_schedulesigaction.c:61, up_schedule_sigaction():
 * "This operation should not cause the task to be unblocked nor should it
 * cause any immediate execution of sigdeliver"); only a signal a task sends
 * to itself runs at once, which is what a self-suspend relies on.  A signal
 * whose action is not installed yet is dropped silently
 * (sched/signal/sig_dispatch.c:117-127: "There is no default action").
 * Three orderings therefore have to be covered:
 *
 *   1. the resume is issued before the action was delivered - the resume
 *      must not be lost and the target must not park afterwards;
 *   2. the suspend is issued before the target's first dispatch - the
 *      signal is dropped by the kernel, the request must not be lost;
 *   3. suspend and resume overlap, or a suspend follows a resume before
 *      the target ran again - each request must take effect once, and
 *      only while it is not released.
 *
 * The protocol, and the invariant it establishes:
 *
 *   - every suspend takes the next number in suspend_req, *before* the
 *     signal is sent;
 *   - suspend_rel is the highest request released so far.  A resume (and
 *     a deinit, and a suspend whose acknowledgement timed out) raises it
 *     to the latest request; it never moves back;
 *   - a request is outstanding while suspend_rel is behind suspend_req.
 *     The target parks only for an outstanding request and stays parked
 *     only while suspend_rel is behind the request it parked for, so a
 *     resume that beats the handler cancels the suspend, and a suspend
 *     that follows a resume before the target ran again is a new request
 *     with a new number, not the released one;
 *   - suspend_sem carries the wake-up.  Every resume posts it, also when
 *     the target is not parked (yet), so a release that arrives before
 *     the target parks is not lost.  The parked target re-checks
 *     suspend_rel after every wake-up, so a token that belongs to an
 *     already released request cannot end a later suspension.
 *
 * Net effect: a suspend followed by a resume always leaves the target
 * runnable, whatever the ordering, and the target parks exactly while a
 * suspend is outstanding.
 */

/* Bounded acknowledgement wait.  The target has to be dispatched for the
 * signal action to run; a target that never runs (a higher priority task
 * holds the CPU) must not block the suspending thread forever.  Each
 * iteration yields the CPU, so the target gets its dispatch.  usleep()
 * rounds up to whole ticks, so the bound is ACK_TRIES ticks or more, not
 * ACK_US * ACK_TRIES microseconds. */
#define OVE_NUTTX_SUSPEND_ACK_US 100u
#define OVE_NUTTX_SUSPEND_ACK_TRIES 100u

/* Sequence order that survives the 32-bit wrap. */
static bool seq_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

/* The outstanding suspend request, if any: true and its number in @seq
 * when a request has not been released yet.  The caller parks for it. */
static bool suspend_claim(struct ove_thread *t, uint32_t *seq)
{
	uint32_t req = __atomic_load_n(&t->suspend_req, __ATOMIC_ACQUIRE);

	if (!seq_before(__atomic_load_n(&t->suspend_rel, __ATOMIC_ACQUIRE), req))
		return false;
	*seq = req;
	return true;
}

/* Release every request up to @seq.  suspend_rel only moves forward, so
 * a late release of an older request cannot undo a newer one. */
static void suspend_release_upto(struct ove_thread *t, uint32_t seq)
{
	uint32_t rel = __atomic_load_n(&t->suspend_rel, __ATOMIC_ACQUIRE);

	while (seq_before(rel, seq) &&
	       !__atomic_compare_exchange_n(&t->suspend_rel, &rel, seq, false, __ATOMIC_ACQ_REL,
					    __ATOMIC_ACQUIRE)) {
	}
}

/* Park the target until request @seq is released (see suspend_claim()). */
static void suspend_park(struct ove_thread *t, uint32_t seq)
{
	SET_STATE(t, OVE_THREAD_STATE_SUSPENDED);
	/* The release token may already be there (the resume arrived before
	 * this task parked), so the release is checked before and after every
	 * wait.  A wait that returns EINTR (another signal) simply repeats.
	 * A releaser of an older request may have marked this task READY
	 * while it is still parked for @seq; every releaser posts after that
	 * store, so the task wakes here and says SUSPENDED again. */
	while (seq_before(__atomic_load_n(&t->suspend_rel, __ATOMIC_ACQUIRE), seq)) {
		nxsem_wait(&t->suspend_sem);
		if (seq_before(__atomic_load_n(&t->suspend_rel, __ATOMIC_ACQUIRE), seq) &&
		    __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != OVE_THREAD_STATE_SUSPENDED) {
			SET_STATE(t, OVE_THREAD_STATE_SUSPENDED);
		}
	}
	/* The task runs again from here on. */
	SET_STATE(t, OVE_THREAD_STATE_RUNNING);
}

/* After a release: the target is no longer suspended unless a newer
 * request is still outstanding (it parked, or is about to park, for that
 * one).  A target that parks for a newer request after this check
 * corrects the state itself in suspend_park(). */
static void suspend_mark_released(struct ove_thread *t)
{
	if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED &&
	    !seq_before(__atomic_load_n(&t->suspend_rel, __ATOMIC_ACQUIRE),
			__atomic_load_n(&t->suspend_req, __ATOMIC_ACQUIRE))) {
		SET_STATE(t, OVE_THREAD_STATE_READY);
	}
}

static void sigusr1_handler(int sig)
{
	(void)sig;
	struct ove_thread *t = tls_get_current();
	if (t == NULL || !__atomic_load_n(&t->suspend_inited, __ATOMIC_ACQUIRE)) {
		return;
	}
	/* Claim the request: a resume that raced this handler cancelled the
	 * suspend already, so the target must not park. */
	uint32_t seq;
	if (!suspend_claim(t, &seq)) {
		return;
	}
	suspend_park(t, seq);
}

/*
 * Install the suspend/resume signal handler in the calling task's group.
 * Signal actions are per task group in NuttX and every ove thread is its
 * own task with its own group, so this must run inside task_wrapper —
 * installing it from the creating task (as it used to) left the new task
 * without a handler and ove_thread_suspend()'s kill(SIGUSR1) a no-op.
 */
static void install_sigusr1_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigusr1_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	int ret = sigaction(SIGUSR1, &sa, NULL);
	DEBUGASSERT(ret == 0);
	(void)ret;
}

static int task_wrapper(int argc, char *argv[])
{
	struct ove_thread *t;

	(void)argc;
	t = (struct ove_thread *)strtoul(argv[1], NULL, 0);
	/* The TLS pointer has to be published *before* the handler can run: a
	 * SIGUSR1 delivered between the handler installation and the TLS
	 * assignment would find no thread (tls_get_current() == NULL), and
	 * the suspend request would be consumed and lost. */
	tls_set_current(t);
	install_sigusr1_handler();

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	/* Init the tracker in the owning task so last_ts_us is anchored at
	 * the actual scheduling moment, not at ove_thread_init() time — and
	 * before obs_register() makes it visible to the scheduler hooks. */
	ove_state_track_init(&t->st, OVE_THREAD_STATE_READY);
	obs_register(t);
#endif
	SET_STATE(t, OVE_THREAD_STATE_RUNNING);

	/* A suspend that was requested before this task's first dispatch must
	 * not be lost: NuttX drops a signal whose action is not installed yet
	 * (sched/signal/sig_dispatch.c:117-127), so the request is honoured
	 * here, before the entry function runs. */
	uint32_t seq;
	if (suspend_claim(t, &seq)) {
		suspend_park(t, seq);
		SET_STATE(t, OVE_THREAD_STATE_RUNNING);
	}

	t->entry(t->arg);
	/* Close the lane and leave the note table BEFORE TERMINATED is
	 * published: ove_thread_deinit() skips the join on TERMINATED and
	 * the owner may free t right away, so no note may find t after
	 * that store. */
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	OBS_TRANSITION(t, OVE_THREAD_STATE_TERMINATED);
	obs_unregister(t);
#endif
	__atomic_store_n(&t->state, OVE_THREAD_STATE_TERMINATED, __ATOMIC_RELEASE);
	nxsem_post(&t->done_sem);
	return 0;
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

static int thread_start(struct ove_thread *t, const char *name, ove_thread_fn entry, void *arg,
			ove_prio_t priority, size_t stack_size, void *stack)
{
	char addr_str[20];
	int pid;

	t->entry = entry;
	t->arg = arg;
	__atomic_store_n(&t->state, OVE_THREAD_STATE_READY, __ATOMIC_RELEASE);
	t->name = name; /* caller-owned string, retained for trace descriptors */
	nxsem_init(&t->done_sem, 0, 0);
	/* The suspend semaphore is created together with the thread, not on
	 * the first suspend: the handshake publishes the request before the
	 * signal, so a resume may have to post the semaphore before any
	 * suspend happened.  The flag is published before the task exists,
	 * so the handler of the new task always sees it. */
	nxsem_init(&t->suspend_sem, 0, 0);
	__atomic_store_n(&t->suspend_inited, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&t->suspend_req, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&t->suspend_rel, 0, __ATOMIC_RELEASE);

	if (stack_size == 0) {
		stack_size = 2048;
	}

	(void)snprintf(addr_str, sizeof(addr_str), "0x%lx", (unsigned long)(uintptr_t)t);
	{
		char *argv_args[] = {addr_str, NULL};
		/*
		 * NuttX zero-heap reality: each ove_thread_create involves
		 * a kmm allocation that we cannot eliminate from
		 * application code:
		 *
		 *   - task_create_with_stack() still kmm-allocates the TCB.
		 *     Supplying the caller's stack avoids the separate stack
		 *     allocation, but sched/group/group_create.c:
		 *     group_allocate() which kmm_zallocs sizeof(task_group_s)
		 *     for every TCB_FLAG_TTYPE_TASK.
		 *
		 *   - TCB_FLAG_TTYPE_KERNEL shares g_kthread_group and skips
		 *     the per-thread group_allocate kmm_zalloc, but the
		 *     kernel-thread launch path in CONFIG_BUILD_FLAT diverges
		 *     from the user-task path (different argv stub, no
		 *     nxtask_startup wrapper) and needs more careful setup
		 *     than a simple flag flip.  Verified empirically: kernel
		 *     threads end up in TSTATE_INVALID without running their
		 *     entry function.
		 *
		 *   - task_wrapper() installs the task's SIGUSR1 action in
		 *     its own group; sched/signal/sig_action.c takes it from
		 *     a pool of CONFIG_SIG_PREALLOC_ACTIONS and kmm-allocates
		 *     a block of actions when the pool is empty.
		 *
		 * Bottom line: dynamic ove_thread_create on NuttX inherently
		 * touches the kernel mm region.  Apps that want the heap-
		 * lock guarantee on NuttX must structure all thread creation
		 * to happen during ove_main() (before ove_run() locks); the
		 * benchmark cannot follow that pattern because measuring
		 * dynamic create/destroy latency IS its purpose, so it
		 * bypasses ove_run() (calls ove_thread_start_scheduler()
		 * directly) on every backend.  See task #18 in the project
		 * tracker for the longer-term fix (nxtask_init with kernel-
		 * thread launch sequence).
		 */
		pid = task_create_with_stack(name ? name : "ove_thread",
					     ove_nuttx_map_priority(priority), stack,
					     (int)stack_size, task_wrapper, argv_args);
		if (pid < 0) {
			nxsem_destroy(&t->done_sem);
			nxsem_destroy(&t->suspend_sem);
			return OVE_ERR_NO_MEMORY;
		}
	}

	t->pid = pid;
	__atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
	_register_thread(t);

	if (first_thread == NULL) {
		first_thread = t;
	}

	return OVE_OK;
}

int ove_thread_init(ove_thread_t *handle, ove_thread_storage_t *storage, const char *name,
		    ove_thread_fn entry, void *arg, ove_prio_t priority, size_t stack_size,
		    void *stack)
{
	if (handle == NULL || storage == NULL || entry == NULL ||
	    (unsigned)priority > OVE_PRIO_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* AAPCS requires 8-byte alignment at public function boundaries; a
	 * misaligned stack faults on first entry.  Sanctioned helpers in
	 * include/ove/storage.h apply aligned(8); this backstops any hand-
	 * rolled array that skips them. */
	if (stack != NULL && ((uintptr_t)stack & 7u) != 0u) {
		return OVE_ERR_INVALID_PARAM;
	}
	/* NuttX can use the caller's stack, but still allocates a TCB and task
	 * group internally. The latter remains the zero-heap limitation
	 * documented in thread_start(). */

	memset(storage, 0, sizeof(*storage));
	storage->generation = ove_thread_next_generation(&g_thread_generation);
	int ret = thread_start(storage, name, entry, arg, priority, stack_size, stack);
	if (ret != OVE_OK) {
		return ret;
	}

	*handle = storage;
	return OVE_OK;
}

int ove_thread_deinit(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	if (__atomic_load_n(&handle->started, __ATOMIC_ACQUIRE)) {
		/* Release the target so it can finish: release every suspend
		 * request, so a handler that has not run yet does not park, and
		 * wake a target that is (or is about to be) parked in the SIGUSR1
		 * suspend loop.  The release
		 * is unconditional - a target that has not parked yet would
		 * otherwise park after this point and never be woken. */
		if (__atomic_load_n(&handle->suspend_inited, __ATOMIC_ACQUIRE)) {
			suspend_release_upto(handle, __atomic_load_n(&handle->suspend_req,
								     __ATOMIC_ACQUIRE));
			suspend_mark_released(handle);
			/* At most one token may be outstanding (see
			 * ove_thread_resume()): drain the token of an earlier
			 * release before this one is posted. */
			(void)nxsem_trywait(&handle->suspend_sem);
			nxsem_post(&handle->suspend_sem);
		}
		/* Wait for thread to finish naturally (join) */
		if (__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) !=
		    OVE_THREAD_STATE_TERMINATED) {
			nxsem_wait_uninterruptible(&handle->done_sem);
		}
		/* done_sem is posted from the task's last lines: the task then
		 * still runs NuttX's exit path on its stack, and a task below
		 * the caller is preempted by the post right there.  The caller
		 * may reuse the storage and the stack as soon as this returns
		 * (ove_thread_init() takes both from it), so wait until the TCB
		 * is released: on one CPU, _exit() releases it and switches
		 * away in one critical section.  kill(pid, 0) cannot tell: it
		 * fails as soon as the exit starts, and the exit hooks can
		 * block.  The pid still names this task: NuttX hands pids out
		 * in increasing order and reuses one only after the counter
		 * wraps. */
#ifdef CONFIG_SMP
#error "ove_thread_deinit(): a released TCB does not prove the stack is free on SMP"
#endif
		while (nxsched_get_tcb(handle->pid) != NULL) {
			usleep(1000);
		}
	}

	_unregister_thread(handle);

	if (first_thread == handle) {
		first_thread = NULL;
	}

	if (__atomic_load_n(&handle->suspend_inited, __ATOMIC_ACQUIRE)) {
		nxsem_destroy(&handle->suspend_sem);
	}
	nxsem_destroy(&handle->done_sem);
	return OVE_OK;
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_THREAD
int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t priority, size_t stack_size)
{
	struct ove_thread *t;
	int ret;

	if (handle == NULL || entry == NULL || (unsigned)priority > OVE_PRIO_MAX) {
		return OVE_ERR_INVALID_PARAM;
	}

	t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (t == NULL) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(t, 0, sizeof(*t));
	t->generation = ove_thread_next_generation(&g_thread_generation);

	ret = thread_start(t, name, entry, arg, priority, stack_size, NULL);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(t);
		return ret;
	}

	*handle = t;
	return OVE_OK;
}

int ove_thread_destroy(ove_thread_t handle)
{
	int ret = ove_check_param(handle);
	if (ret)
		return ret;

	ret = ove_thread_deinit(handle);
	OVE_BACKEND_FREE(handle);
	return ret;
}
#endif /* OVE_HEAP_THREAD */

ove_thread_t ove_thread_get_self(void)
{
	return tls_get_current();
}

void ove_thread_set_priority(ove_thread_t handle, ove_prio_t prio)
{
	struct sched_param param;
	if ((unsigned)prio > OVE_PRIO_MAX)
		return;
	param.sched_priority = ove_nuttx_map_priority(prio);

	pid_t pid = (handle != NULL) ? handle->pid : getpid();
	sched_setparam(pid, &param);
}

void ove_thread_sleep_ms(uint32_t ms)
{
	usleep(ms * 1000U);
}

void ove_thread_yield(void)
{
	sched_yield();
}

void ove_thread_start_scheduler(void)
{
#ifdef __NuttX__
	/* Block until first thread exits (keeps init task alive).
	 * Threads were already created by ove_thread_init() via task_create;
	 * they begin running once this task yields or blocks. */
	if (first_thread != NULL) {
		nxsem_wait_uninterruptible(&first_thread->done_sem);
	}
#endif
	/* On Linux, POSIX threads run immediately — no scheduler to start */
}

void ove_thread_suspend(ove_thread_t handle)
{
	struct ove_thread *t = handle;
	unsigned int i;

	if (t == NULL || !__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) {
		return;
	}
	if (!__atomic_load_n(&t->suspend_inited, __ATOMIC_ACQUIRE)) {
		/* The suspend semaphore is created with the thread; a thread
		 * that never got one cannot be suspended. */
		return;
	}
	if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_TERMINATED) {
		/* The thread already ended - it cannot be suspended. */
		return;
	}

	/* Publish the request *before* the signal.  The target may be
	 * dispatched between the kill() and the handler, and a suspend that
	 * was requested before the target's first dispatch is honoured by
	 * task_wrapper().  The request takes a new number, so a release of
	 * an earlier request cannot cancel it. */
	uint32_t seq = __atomic_add_fetch(&t->suspend_req, 1, __ATOMIC_ACQ_REL);
	kill(t->pid, SIGUSR1);

	/* A thread that suspends itself cannot wait for its own
	 * acknowledgement - it parks in the handler. */
	if (t == tls_get_current()) {
		return;
	}

	/* Wait (bounded) for the acknowledgement: the target sets its state
	 * to SUSPENDED when it parks.  The wait yields the CPU, so the
	 * target gets its dispatch. */
	for (i = 0;; i++) {
		if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED) {
			return;
		}
		if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_TERMINATED) {
			/* The thread ended while the suspend was in flight. */
			break;
		}
		if (!seq_before(__atomic_load_n(&t->suspend_rel, __ATOMIC_ACQUIRE), seq)) {
			/* A resume cancelled the suspend. */
			break;
		}
		/* The state is checked once more after the last sleep, so a
		 * target that parks during it is not taken for a timeout. */
		if (i == OVE_NUTTX_SUSPEND_ACK_TRIES)
			break;
		usleep(OVE_NUTTX_SUSPEND_ACK_US);
	}

	/* Not acknowledged: the target never suspended.  Release the request,
	 * so that a handler that runs later does not park, and wake the
	 * target if it parked for it just before the timeout - nobody would
	 * be left to wake it.
	 *
	 * ove_thread_suspend() returns void (include/ove/thread.h), so the
	 * failure is reported through the thread's state: it stays
	 * READY/RUNNING and ove_thread_get_state() does not report
	 * SUSPENDED. */
	{
		suspend_release_upto(t, seq);
		suspend_mark_released(t);
		/* At most one token may be outstanding (see
		 * ove_thread_resume()): drain a token that an earlier release
		 * left behind before the token of this release is posted. */
		(void)nxsem_trywait(&t->suspend_sem);
		nxsem_post(&t->suspend_sem);
	}
}

void ove_thread_resume(ove_thread_t handle)
{
	struct ove_thread *t = handle;

	if (t == NULL || !__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) {
		return;
	}
	if (!__atomic_load_n(&t->suspend_inited, __ATOMIC_ACQUIRE)) {
		return;
	}

	/* Release the target unconditionally.  A resume that races the
	 * suspend must not be lost: every request up to the latest one is
	 * released (a handler that has not run yet does not park, a target
	 * that is already parked leaves its wait) and the semaphore is posted
	 * (a target that is about to park finds the token). */
	suspend_release_upto(t, __atomic_load_n(&t->suspend_req, __ATOMIC_ACQUIRE));
	suspend_mark_released(t);
	/* At most one token may be outstanding: a resume that arrives
	 * before its target parked leaves a token behind, and every further
	 * resume would pile one token on top of it.  An undrained
	 * semaphore would fill up (nxsem_post() fails with -EOVERFLOW at
	 * SEM_VALUE_MAX) and a release would be lost, so the token of the
	 * previous release is drained before the new one is posted.  The
	 * drain cannot steal the token of a parked target: a target that
	 * waits on the semaphore holds the count at zero (or below), so
	 * nxsem_trywait() finds nothing to take. */
	(void)nxsem_trywait(&t->suspend_sem);
	nxsem_post(&t->suspend_sem);
}

void ove_thread_request_stop(ove_thread_t handle)
{
	if (handle)
		__atomic_store_n(&handle->stop_requested, 1, __ATOMIC_RELEASE);
}

bool ove_thread_should_stop(ove_thread_t handle)
{
	if (!handle)
		return false;
	return __atomic_load_n(&handle->stop_requested, __ATOMIC_ACQUIRE) != 0;
}

#ifdef CONFIG_OVE_THREAD_STATE_STATS
/* ── Scheduler-driven observed state (sched_note driver) ────────────
 *
 * CONFIG_SCHED_INSTRUMENTATION_SWITCH calls the registered note drivers'
 * suspend(tcb) when a task leaves the CPU, ready(tcb) when it is inserted
 * into the ready/pending list, and resume(tcb) when it takes the CPU,
 * inside the scheduler's critical section. Preemption shows as READY and
 * every wait (ove API, kernel, drivers, LVGL) as BLOCKED; the ready note
 * splits a wake from BLOCKED to READY so wake-to-run latency is visible.
 * The ready op needs the small sched_note extension in
 * config/patches/nuttx/0002-sched-note-let-drivers-observe-wakeups.patch.
 *
 * Tasks are mapped to their ove_thread through a small pid table filled
 * by task_wrapper(); kernel and other tasks are not tracked. The table and
 * the tracker are only touched inside critical sections.
 *
 * NuttX reports the end of a task only after task_wrapper() has returned
 * (sched/task/task_exit.c: nxsched_switch_context() raises the suspend
 * note of the exiting task, sched_note_stop(dtcb) the stop note), i.e.
 * after the thread has closed its own lane and left the table.  The TCB
 * of the task is therefore remembered: it is the only way to tell the
 * notes of a tracked thread apart from the notes of a kernel thread once
 * the thread is gone.  It moves to the pending table (obs_pending[]) when
 * the thread leaves the table of the tracked threads, so that a
 * registration never takes over a slot that still carries it.  The exit of
 * a tracked thread must not be reported through the kernel-thread path:
 * the guest never described the tid of a TCB, so the dashboard would draw
 * an empty lane for it.
 */
#define OBS_MAX_TASKS 32

/* The TCBs of the tracked threads whose stop note has not arrived yet.
 *
 * A thread that ends normally leaves the table of the tracked threads
 * (obs_unregister()), but NuttX raises the stop note of its task only
 * afterwards, so the TCB has to survive until that note arrives: it is
 * the only way to tell the notes of a tracked thread apart from the notes
 * of a kernel thread once the thread is gone.
 *
 * The pending TCBs live in this separate table and not in the table of the
 * tracked threads: a registration must never take over a slot that still
 * carries a pending TCB, because that would destroy the TCB the stop note
 * has to be recognised by, and the exit of that thread would then be
 * reported through the kernel-thread path (a lane the guest never
 * described). */
#define OBS_MAX_PENDING 32

static struct {
	pid_t pid;
	struct ove_thread *t;
	/* The TCB of the task this entry belongs to.  It is kept here while
	 * the thread runs and moves to obs_pending[] when the thread leaves
	 * the table, so that the slot can be reused by the next
	 * registration. */
	FAR struct tcb_s *tcb;
} obs_tasks[OBS_MAX_TASKS];

/* The TCBs whose stop note has not arrived yet (see above).  A
 * registration never touches this table. */
static FAR struct tcb_s *obs_pending[OBS_MAX_PENDING];

static void obs_transition(struct ove_thread *t, int s)
{
	int prev = t->st.cur_state;
	if (prev == s || prev == OVE_THREAD_STATE_TERMINATED)
		return;
	/* One clock read, shared by the trace record and the tracker. */
	uint64_t now = ove_state_stats_now_us();
	ove_trace_emit_state_at((uintptr_t)t, prev, s, now);
	ove_state_track_transition_at(&t->st, s, now);
}

static struct ove_thread *obs_find(pid_t pid)
{
	for (int i = 0; i < OBS_MAX_TASKS; i++)
		if (obs_tasks[i].t && obs_tasks[i].pid == pid)
			return obs_tasks[i].t;
	return NULL;
}

/* True when @tcb is the TCB of a task the scheduler-note driver tracked.
 * The TCB of a thread stays in the pending table after the thread left
 * the table of the tracked threads, until the stop note of the task
 * arrives - the notes NuttX raises for the exit of the task come after
 * task_wrapper() returned. */
static bool obs_is_tracked_tcb(FAR struct tcb_s *tcb)
{
	if (tcb == NULL)
		return false;
	for (int i = 0; i < OBS_MAX_TASKS; i++)
		if (obs_tasks[i].tcb == tcb)
			return true;
	for (int i = 0; i < OBS_MAX_PENDING; i++)
		if (obs_pending[i] == tcb)
			return true;
	return false;
}

/* Consume the TCB of a tracked task.  NuttX raises the stop note of a task
 * exactly once, so the TCB is only needed until the note arrives; taking
 * it here also keeps a recycled TCB address from being mistaken for a
 * tracked one later on.  Returns true when @tcb belongs to a task the
 * scheduler-note driver tracked. */
static bool obs_take_tracked_tcb(FAR struct tcb_s *tcb)
{
	if (tcb == NULL)
		return false;
	for (int i = 0; i < OBS_MAX_TASKS; i++) {
		if (obs_tasks[i].tcb == tcb) {
			obs_tasks[i].tcb = NULL;
			return true;
		}
	}
	for (int i = 0; i < OBS_MAX_PENDING; i++) {
		if (obs_pending[i] == tcb) {
			obs_pending[i] = NULL;
			return true;
		}
	}
	return false;
}

/* The ove_thread running as task @pid, or NULL (kernel / other tasks).
 * Used by trace descriptor enumeration for the shared thread identity. */
struct ove_thread *ove_nuttx_thread_by_pid(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	struct ove_thread *t = obs_find(pid);
	leave_critical_section(flags);
	return t;
}

static void obs_note_suspend(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	/* armv7-m/armv8-m arm_doirq() calls nxsched_switch_context(from, to)
	 * after EVERY interrupt, also when from == to.  A task that really
	 * leaves the CPU is never still RUNNING here (the scheduler has made
	 * it READYTORUN, INVALID on exit, or a wait state), so a RUNNING task
	 * did not switch: ignore the note and the matching resume is a
	 * no-op. */
	if (tcb->task_state == TSTATE_TASK_RUNNING)
		return;
	struct ove_thread *t = obs_find(tcb->pid);

	int next;
	if (t && __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_SUSPENDED)
		next = OVE_THREAD_STATE_SUSPENDED; /* parked in the SIGUSR1 suspend loop */
	else if (tcb->task_state == TSTATE_TASK_READYTORUN)
		next = OVE_THREAD_STATE_READY; /* preempted or yielded */
	else
		next = OVE_THREAD_STATE_BLOCKED;
	if (t)
		obs_transition(t, next);
	else if (!obs_is_tracked_tcb(tcb))
		/* A kernel thread (or a task the driver never tracked): it has a
		 * lane of its own, so the note belongs there.  A tracked task
		 * whose thread already ended must not be reported there: the
		 * guest never described the tid of a TCB. */
		ove_trace_emit_kthread_state((uintptr_t)tcb, next, OVE_TRACE_KT_ALWAYS);
}

static void obs_note_resume(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	struct ove_thread *t = obs_find(tcb->pid);
	if (t)
		obs_transition(t, OVE_THREAD_STATE_RUNNING);
	else if (!obs_is_tracked_tcb(tcb))
		ove_trace_emit_kthread_state((uintptr_t)tcb, OVE_THREAD_STATE_RUNNING,
					     OVE_TRACE_KT_ALWAYS);
}

static void obs_note_stop_locked(FAR struct tcb_s *tcb)
{
	/* The note of a task comes exactly once, so the TCB of a tracked task
	 * is consumed here. */
	bool tracked = obs_take_tracked_tcb(tcb);
	struct ove_thread *t = obs_find(tcb->pid);
	if (t) {
		/* The task was terminated without task_wrapper() reaching its
		 * end: the lane of the thread is still open, close it here.
		 *
		 * The slot of the thread is released as well: the thread
		 * will never unregister itself (obs_unregister() only runs
		 * when task_wrapper() returns), so the slot would leak and it
		 * would keep a pointer to a struct ove_thread that its owner
		 * may free - obs_find() could then hand that stale pointer
		 * out for the pid of the dead thread. */
		obs_transition(t, OVE_THREAD_STATE_TERMINATED);
		for (int i = 0; i < OBS_MAX_TASKS; i++) {
			if (obs_tasks[i].t != t) {
				continue;
			}
			obs_tasks[i].t = NULL;
			obs_tasks[i].pid = 0;
			obs_tasks[i].tcb = NULL;
			break;
		}
		return;
	}
	if (tracked) {
		/* The thread ended the normal way: task_wrapper() closed its
		 * lane and left the table before NuttX raised this note.
		 * Reporting the exit through the kernel-thread path would
		 * invent a lane for a tid the guest never described. */
		return;
	}
	/* A kernel thread (or any other task the driver never tracked): it
	 * has a lane of its own, so the exit belongs there and frees its
	 * kernel-thread slot. */
	ove_trace_emit_kthread_state((uintptr_t)tcb, OVE_THREAD_STATE_TERMINATED,
				     OVE_TRACE_KT_ALWAYS);
}

static void obs_note_stop(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	/* Unlike the switch notes, the stop note can come outside the
	 * scheduler's critical section: task_delete() -> nxtask_terminate()
	 * raises it after leave_critical_section(). */
	irqstate_t flags = enter_critical_section();
	obs_note_stop_locked(tcb);
	leave_critical_section(flags);
}

/* The ready op is patched into the note driver ops (see
 * config/patches/nuttx/0002-sched-note-let-drivers-observe-wakeups.patch),
 * which defines OVE_NUTTX_HAVE_SCHED_NOTE_READY in note_driver.h.  An
 * unpatched kernel has no such op, so the driver is built without it there
 * — the backend still compiles, it just does not see the wake-up edge. */
#ifdef OVE_NUTTX_HAVE_SCHED_NOTE_READY
static void obs_note_ready(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	struct ove_thread *t = obs_find(tcb->pid);
	if (t)
		obs_transition(t, OVE_THREAD_STATE_READY);
	else if (!obs_is_tracked_tcb(tcb))
		/* Only a wake: a preempted kernel thread is READY already. */
		ove_trace_emit_kthread_state((uintptr_t)tcb, OVE_THREAD_STATE_READY,
					     OVE_TRACE_KT_FROM_WAITING);
}
#endif

static const struct note_driver_ops_s obs_note_ops = {
	.stop = obs_note_stop,
	.suspend = obs_note_suspend,
	.resume = obs_note_resume,
#ifdef OVE_NUTTX_HAVE_SCHED_NOTE_READY
	.ready = obs_note_ready,
#endif
};

static struct note_driver_s obs_note_driver = {
	.ops = &obs_note_ops,
};

/* Called by the task itself on entry, before it marks itself RUNNING. */
static void obs_register(struct ove_thread *t)
{
	static bool driver_registered;
	irqstate_t flags = enter_critical_section();
	if (!driver_registered) {
		/* No unregister API exists; one static driver for the system. */
		driver_registered = note_driver_register(&obs_note_driver) == OK;
	}
	/* Claim a slot for the task.  Only a slot that carries neither a
	 * thread nor a pending TCB may be claimed: a pending TCB has to
	 * survive until the stop note of its task arrives, otherwise the
	 * exit of that thread would be reported as the exit of a kernel
	 * thread.  The pending TCBs live in their own table (obs_pending[]),
	 * so a thread that ended normally does not block the registration
	 * of the next one and a registration never overwrites a pending
	 * TCB. */
	int slot = -1;
	for (int i = 0; i < OBS_MAX_TASKS; i++) {
		if (obs_tasks[i].t == NULL && obs_tasks[i].tcb == NULL) {
			slot = i;
			break;
		}
	}
	if (slot >= 0) {
		obs_tasks[slot].pid = getpid();
		obs_tasks[slot].t = t;
		obs_tasks[slot].tcb = nxsched_self();
	}
	leave_critical_section(flags);
	if (slot < 0) {
		/* More threads run at the same time than the table can hold:
		 * the thread is not observed, so its notes are reported
		 * through the kernel-thread path. */
#ifdef __NuttX__
		syslog(LOG_ERR, "ove: the thread table is full, the thread is not observed\n");
#endif
	}
}

static void obs_unregister(struct ove_thread *t)
{
	irqstate_t flags = enter_critical_section();
	/* The thread leaves the table, but the TCB of its task has to
	 * survive: NuttX raises the stop note of the task only after
	 * task_wrapper() returned, and the note must still be recognised as
	 * the note of a tracked thread.  The TCB moves to the pending table
	 * so that the slot of the thread can be reused by the next
	 * registration - the pending table is never used by a registration,
	 * so a pending TCB cannot be overwritten. */
	for (int i = 0; i < OBS_MAX_TASKS; i++) {
		FAR struct tcb_s *tcb;

		if (obs_tasks[i].t != t) {
			continue;
		}
		tcb = obs_tasks[i].tcb;
		obs_tasks[i].t = NULL;
		obs_tasks[i].pid = 0;
		obs_tasks[i].tcb = NULL;
		if (tcb == NULL) {
			break;
		}
		for (int j = 0; j < OBS_MAX_PENDING; j++) {
			if (obs_pending[j] == NULL) {
				obs_pending[j] = tcb;
				tcb = NULL;
				break;
			}
		}
		if (tcb != NULL) {
			/* The pending table is full: the TCB stays in the
			 * slot of the thread.  The slot is not free then (it
			 * still carries the TCB), so a registration does not
			 * take it over. */
			obs_tasks[i].tcb = tcb;
		}
		break;
	}
	leave_critical_section(flags);
}
#endif

int ove_thread_get_stack_headroom(ove_thread_t handle, size_t *headroom_bytes)
{
	if (!headroom_bytes)
		return OVE_ERR_INVALID_PARAM;
	*headroom_bytes = 0;
	if (!handle)
		return OVE_ERR_INVALID_PARAM;

	/* The public NuttX task API does not expose a safe stack high-water
	 * query for this caller-owned thread handle. */
	return OVE_ERR_NOT_SUPPORTED;
}

size_t ove_thread_get_stack_usage(ove_thread_t handle)
{
	size_t headroom = 0;
	return ove_thread_get_stack_headroom(handle, &headroom) == OVE_OK ? headroom : 0;
}

ove_thread_state_t ove_thread_get_state(ove_thread_t handle)
{
	if (handle == NULL) {
		return OVE_THREAD_STATE_TERMINATED;
	}

#ifdef __NuttX__
	/* Quick liveness check — no procfs file I/O */
	if (__atomic_load_n(&handle->state, __ATOMIC_ACQUIRE) == OVE_THREAD_STATE_RUNNING) {
		if (kill(handle->pid, 0) != 0) {
			return OVE_THREAD_STATE_TERMINATED;
		}
	}
#endif

	return __atomic_load_n(&handle->state, __ATOMIC_ACQUIRE);
}

int ove_thread_get_runtime_stats(ove_thread_t handle, struct ove_thread_stats *stats)
{
	(void)handle;
	stats->runtime_us = 0;
	stats->cpu_percent_x100 = 0;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	if (!stats)
		return OVE_ERR_INVALID_PARAM;
	/* mallinfo() is technically not reentrant in POSIX libc, but on NuttX
	 * it is the canonical heap-stats API and the implementation grabs
	 * the kmm lock internally.  This is the ove_sys_get_mem_stats
	 * surface point — concurrent callers see consistent snapshots. */
	struct mallinfo mi = mallinfo(); /* NOLINT(concurrency-mt-unsafe) */
	stats->total = (size_t)mi.arena;
	stats->used = (size_t)mi.uordblks;
	stats->free = (size_t)mi.fordblks;
	stats->peak_used = (size_t)mi.usmblks;
	return OVE_OK;
}

struct _nuttx_list_ctx {
	struct ove_thread_info *out;
	struct _nuttx_list_snapshot *snapshot;
	size_t max;
	size_t count;
	bool overflow;
};

/* nxsched_foreach() invokes its callback while interrupts are disabled. Keep
 * that callback to a bounded scalar snapshot: stack-color scanning can touch
 * hundreds of KiB for a personality task and clock_cpuload() takes another
 * scheduler lock. A TCB cannot be pinned across the traversal, so stack_used
 * remains zero (unknown) instead of dereferencing a possibly exited task. */
struct _nuttx_list_snapshot {
	pid_t pid;
#if CONFIG_TASK_NAME_SIZE > 0
	char name[CONFIG_TASK_NAME_SIZE + 1];
#endif
};

#if defined(CONFIG_ARCH_PERF_EVENTS)

/* Exact per-task execution time. The context-switch note hook folds the elapsed
 * DWT cycles into the outgoing task. ove_thread_list() also folds the current
 * interval, so a CPU-bound task that never switches still advances on every
 * /proc refresh. Both paths run with interrupts masked while touching 64-bit
 * counters on this 32-bit core. */
#define OVE_NX_RUNTIME_MAX 32
static struct {
	uint64_t cycles; /* first: no padding before it on a 32-bit core */
	pid_t pid;
	bool used;
} g_nx_runtime[OVE_NX_RUNTIME_MAX];
static pid_t g_nx_runtime_current = -1;
static int g_nx_runtime_current_index = -1;
static uint32_t g_nx_runtime_last;
static uint64_t g_nx_runtime_total;
static bool g_nx_runtime_ready;
static bool g_nx_runtime_overflow;

static int _runtime_find(pid_t pid, bool create)
{
	int freeidx = -1;

	for (int i = 0; i < OVE_NX_RUNTIME_MAX; i++) {
		if (g_nx_runtime[i].used && g_nx_runtime[i].pid == pid)
			return i;
		if (!g_nx_runtime[i].used && freeidx < 0)
			freeidx = i;
	}
	if (!create || freeidx < 0) {
		if (create)
			g_nx_runtime_overflow = true;
		return -1;
	}
	g_nx_runtime[freeidx].used = true;
	g_nx_runtime[freeidx].pid = pid;
	g_nx_runtime[freeidx].cycles = 0;
	return freeidx;
}

static void _runtime_fold(uint32_t now)
{
	if (!g_nx_runtime_ready) {
		g_nx_runtime_last = now;
		g_nx_runtime_ready = true;
		return;
	}

	uint32_t delta = now - g_nx_runtime_last;
	g_nx_runtime_total += delta;
	if (g_nx_runtime_current_index >= 0)
		g_nx_runtime[g_nx_runtime_current_index].cycles += delta;
	g_nx_runtime_last = now;
}

void ove_nuttx_runtime_reset(pid_t current_pid)
{
	irqstate_t flags = enter_critical_section();
	memset(g_nx_runtime, 0, sizeof(g_nx_runtime));
	g_nx_runtime_current = current_pid;
	g_nx_runtime_current_index = current_pid >= 0 ? _runtime_find(current_pid, true) : -1;
	g_nx_runtime_total = 0;
	g_nx_runtime_last = (uint32_t)up_perf_gettime();
	g_nx_runtime_ready = true;
	g_nx_runtime_overflow = false;
	leave_critical_section(flags);
}

void ove_nuttx_runtime_start(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	int idx = _runtime_find(pid, true);
	if (idx >= 0)
		g_nx_runtime[idx].cycles = 0;
	leave_critical_section(flags);
}

void ove_nuttx_runtime_stop(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	int idx = _runtime_find(pid, false);
	if (idx >= 0) {
		if (idx == g_nx_runtime_current_index) {
			_runtime_fold((uint32_t)up_perf_gettime());
			g_nx_runtime_current = -1;
			g_nx_runtime_current_index = -1;
		}
		memset(&g_nx_runtime[idx], 0, sizeof(g_nx_runtime[idx]));
	}
	leave_critical_section(flags);
}

void ove_nuttx_runtime_switch(pid_t next_pid)
{
	/* sched_note_resume() runs from NuttX's scheduler switch path with the
	 * scheduler state protected. Keep this hot path free of a redundant
	 * nested critical section: it runs for every 1 kHz scope wakeup. */
	_runtime_fold((uint32_t)up_perf_gettime());
	g_nx_runtime_current = next_pid;
	g_nx_runtime_current_index = next_pid >= 0 ? _runtime_find(next_pid, true) : -1;
}

void ove_nuttx_runtime_snapshot(void)
{
	irqstate_t flags = enter_critical_section();
	_runtime_fold((uint32_t)up_perf_gettime());
	leave_critical_section(flags);
}

int ove_nuttx_runtime_get(pid_t pid, uint64_t *task_cycles, uint64_t *total_cycles)
{
	int ret = -1;
	irqstate_t flags = enter_critical_section();
	int idx = _runtime_find(pid, false);
	if (idx >= 0) {
		if (task_cycles)
			*task_cycles = g_nx_runtime[idx].cycles;
		ret = 0;
	}
	if (total_cycles)
		*total_cycles = g_nx_runtime_total;
	leave_critical_section(flags);
	return ret;
}

uint64_t ove_nuttx_runtime_cycles_to_us(uint64_t cycles)
{
	uint64_t freq = up_perf_getfreq();
	if (freq == 0)
		return 0;
	/* Quotient/remainder avoids overflowing cycles * 1,000,000 on long runs. */
	return (cycles / freq) * 1000000u + ((cycles % freq) * 1000000u) / freq;
}

#else

void ove_nuttx_runtime_reset(pid_t current_pid)
{
	(void)current_pid;
}

void ove_nuttx_runtime_start(pid_t pid)
{
	(void)pid;
}

void ove_nuttx_runtime_stop(pid_t pid)
{
	(void)pid;
}

void ove_nuttx_runtime_switch(pid_t next_pid)
{
	(void)next_pid;
}

void ove_nuttx_runtime_snapshot(void)
{
}

int ove_nuttx_runtime_get(pid_t pid, uint64_t *task_cycles, uint64_t *total_cycles)
{
	(void)pid;
	if (task_cycles)
		*task_cycles = 0;
	if (total_cycles)
		*total_cycles = 0;
	return -1;
}

uint64_t ove_nuttx_runtime_cycles_to_us(uint64_t cycles)
{
	(void)cycles;
	return 0;
}

#endif

/* NuttX's per-thread cpuload is an exponentially-DECAYED load average (sched_cpuload.c halves every
 * thread's tick count every ~2s), NOT a cumulative runtime. The personality's ps/top stats layer
 * charges DELTAS of running_us expecting it cumulative, so a decayed value plateaus at steady state
 * → per-proc %CPU reads 0. Rebuild a MONOTONIC cumulative running_us by integrating each thread's
 * load fraction (cpu_percent_x100) over the wall-time between ove_thread_list() calls. Keyed by pid;
 * a slot whose thread wasn't seen in a pass is reclaimed, so recreated/exited threads neither leak
 * a slot nor carry a stale total. */
#if !defined(CONFIG_ARCH_PERF_EVENTS)
#define OVE_NX_CPUINT_MAX 32
#else
#define OVE_NX_CPUINT_MAX OVE_NX_RUNTIME_MAX
#endif

static struct _nuttx_list_snapshot g_nx_list_snapshot[OVE_NX_CPUINT_MAX];
static mutex_t g_nx_list_lock = NXMUTEX_INITIALIZER;

#if !defined(CONFIG_ARCH_PERF_EVENTS)
static struct {
	uint64_t cum_us; /* first: 16 bytes an entry instead of 24 */
	pid_t pid;
	bool used;
	bool seen;
} g_nx_cpuint[OVE_NX_CPUINT_MAX];
static clock_t g_nx_cpuint_last;
static bool g_nx_cpuint_overflow;
#endif

#if defined(CONFIG_ARCH_PERF_EVENTS)
static uint32_t _runtime_percent(uint64_t part, uint64_t total)
{
	if (total == 0)
		return 0;

	/* Keep the multiply bounded on multi-month uptimes. Shifting numerator
	 * and denominator together only discards precision far below 0.01%. */
	while (total > UINT64_MAX / 10000u) {
		part >>= 1;
		total >>= 1;
	}
	return (uint32_t)(part * 10000u / total);
}
#endif

static void _nuttx_list_cb(struct tcb_s *tcb, void *arg)
{
	struct _nuttx_list_ctx *ctx = (struct _nuttx_list_ctx *)arg;
	if (ctx->count >= ctx->max) {
		ctx->overflow = true;
		return;
	}

	size_t index = ctx->count;
	struct ove_thread_info *info = &ctx->out[index];
	struct _nuttx_list_snapshot *snapshot = &ctx->snapshot[index];

#if CONFIG_TASK_NAME_SIZE > 0
	memcpy(snapshot->name, tcb->name, sizeof(snapshot->name));
	snapshot->name[sizeof(snapshot->name) - 1u] = '\0';
	info->name = snapshot->name;
#else
	info->name = "?";
#endif
	snapshot->pid = tcb->pid;
	info->identity = (uintptr_t)(uint32_t)tcb->pid;
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	struct ove_thread *ove = ove_nuttx_thread_by_pid(tcb->pid);
	info->trace_id = ove ? (uintptr_t)ove : (uintptr_t)tcb;
	info->generation = ove ? ove->generation : 0u;
#else
	info->trace_id = (uintptr_t)tcb;
	info->generation = 0u;
#endif

	switch (tcb->task_state) {
	case TSTATE_TASK_RUNNING:
		info->state = OVE_THREAD_STATE_RUNNING;
		break;
	case TSTATE_TASK_READYTORUN:
		info->state = OVE_THREAD_STATE_READY;
		break;
	case TSTATE_TASK_INACTIVE:
		info->state = OVE_THREAD_STATE_TERMINATED;
		break;
	default:
		/* All TSTATE_WAIT_* states are blocked */
		info->state = OVE_THREAD_STATE_BLOCKED;
		break;
	}

	info->native_priority = (int)tcb->sched_priority;
	info->priority = (int)ove_nuttx_unmap_priority(info->native_priority);
	info->stack_size = tcb->adj_stack_size;
	info->stack_used = 0;
	info->cpu_percent_x100 = 0;
	info->valid_fields = OVE_THREAD_INFO_VALID_STACK_SIZE;
	memset(&info->state_times, 0, sizeof(info->state_times));

	ctx->count++;
}

/* NuttX has no "suspended" task state: a thread parked in the SIGUSR1
 * suspend loop waits on a semaphore (TSTATE_WAIT_SEM), so the scheduler
 * walk lists it as BLOCKED, while FreeRTOS and Zephyr report SUSPENDED.
 * The suspension is taken from the ove thread's own state, found through
 * the registry by pid.  This runs in task context after the walk: the
 * walk's callback runs with interrupts off and cannot take the registry
 * mutex. */
static void _nuttx_list_mark_suspended(struct ove_thread_info *out,
				       const struct _nuttx_list_snapshot *snapshot, size_t count)
{
	nxmutex_lock(&thread_list_lock);
	for (struct ove_thread *t = ove_backend_thread_registry_head; t; t = t->next) {
		if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != OVE_THREAD_STATE_SUSPENDED)
			continue;
		for (size_t i = 0; i < count; i++) {
			if (snapshot[i].pid == t->pid) {
				out[i].state = OVE_THREAD_STATE_SUSPENDED;
				break;
			}
		}
	}
	nxmutex_unlock(&thread_list_lock);
}

static void _nuttx_list_finish(struct ove_thread_info *info,
			       const struct _nuttx_list_snapshot *snapshot, uint64_t dt_us)
{
#if defined(CONFIG_ARCH_PERF_EVENTS)
	(void)dt_us;
	uint64_t task_cycles;
	uint64_t total_cycles;
	if (ove_nuttx_runtime_get(snapshot->pid, &task_cycles, &total_cycles) == 0) {
		info->state_times.running_us = ove_nuttx_runtime_cycles_to_us(task_cycles);
		info->cpu_percent_x100 = _runtime_percent(task_cycles, total_cycles);
		info->valid_fields |= OVE_THREAD_INFO_VALID_CPU_PERCENT |
				      OVE_THREAD_INFO_VALID_RUNNING_TIME;
	}
#elif !defined(CONFIG_SCHED_CPULOAD_NONE)
	{
		struct cpuload_s cl;
		if (clock_cpuload(snapshot->pid, &cl) == OK && cl.total > 0) {
			uint32_t pct = (uint32_t)((uint64_t)cl.active * 10000U / cl.total);
			info->cpu_percent_x100 = pct;
			info->valid_fields |= OVE_THREAD_INFO_VALID_CPU_PERCENT;
			/* cl.active is a DECAYED average, not cumulative — integrate the load fraction
			 * into a monotonic cumulative running_us (see the g_nx_cpuint note). */
			int idx = -1, freeidx = -1;
			for (int k = 0; k < OVE_NX_CPUINT_MAX; k++) {
				if (g_nx_cpuint[k].used && g_nx_cpuint[k].pid == snapshot->pid) {
					idx = k;
					break;
				}
				if (!g_nx_cpuint[k].used && freeidx < 0)
					freeidx = k;
			}
			if (idx < 0 && freeidx >= 0) {
				idx = freeidx;
				g_nx_cpuint[idx].used = true;
				g_nx_cpuint[idx].pid = snapshot->pid;
				g_nx_cpuint[idx].cum_us = 0;
			}
			if (idx < 0)
				g_nx_cpuint_overflow = true;
			if (idx >= 0) {
				g_nx_cpuint[idx].seen = true;
				g_nx_cpuint[idx].cum_us += (uint64_t)pct * dt_us / 10000U;
				info->state_times.running_us = g_nx_cpuint[idx].cum_us;
				info->valid_fields |= OVE_THREAD_INFO_VALID_RUNNING_TIME;
			}
		}
	}
#endif

#ifdef CONFIG_OVE_THREAD_STATE_STATS
	/* ove-created tasks report all four states from their tracker, so the
	 * Threads view gets a state bar; kernel and other tasks (no tracker)
	 * keep the running-time estimate above.  The note driver updates the
	 * tracker inside the scheduler's critical section, so look the thread
	 * up, read the clock and copy the tracker inside one as well. */
	irqstate_t flags = enter_critical_section();
	struct ove_thread *t = obs_find(snapshot->pid);
	if (t && t->st.cur_state != OVE_THREAD_STATE_TERMINATED) {
		uint64_t times[OVE_STATE_COUNT];
		ove_state_track_read(&t->st, ove_state_stats_now_us(), times);
		info->state_times.running_us = times[OVE_THREAD_STATE_RUNNING];
		info->state_times.ready_us = times[OVE_THREAD_STATE_READY];
		info->state_times.blocked_us = times[OVE_THREAD_STATE_BLOCKED];
		info->state_times.suspended_us = times[OVE_THREAD_STATE_SUSPENDED];
		info->valid_fields |= OVE_THREAD_INFO_VALID_STATE_TIMES;
	}
	leave_critical_section(flags);
#endif
}

int ove_thread_list(struct ove_thread_info *out, size_t max_count, size_t *actual_count)
{
	if (!out) {
		if (actual_count)
			*actual_count = 0;
		return OVE_OK;
	}

	nxmutex_lock(&g_nx_list_lock);
	struct _nuttx_list_ctx ctx = {
		.out = out,
		.snapshot = g_nx_list_snapshot,
		.max = max_count < OVE_NX_CPUINT_MAX ? max_count : OVE_NX_CPUINT_MAX,
		.count = 0,
	};

	/* The decayed-load fallback must integrate its samples into cumulative
	 * time. Hardware perf-counter builds already have cumulative cycles. */
#if !defined(CONFIG_ARCH_PERF_EVENTS)
	clock_t now = clock_systime_ticks();
	uint64_t dt_us =
		(g_nx_cpuint_last == 0) ? 0 : (uint64_t)(now - g_nx_cpuint_last) * USEC_PER_TICK;
	g_nx_cpuint_last = now;
	for (int k = 0; k < OVE_NX_CPUINT_MAX; k++)
		g_nx_cpuint[k].seen = false;
#else
	uint64_t dt_us = 0;
#endif

	ove_nuttx_runtime_snapshot();
	nxsched_foreach(_nuttx_list_cb, &ctx);
	_nuttx_list_mark_suspended(out, g_nx_list_snapshot, ctx.count);
	for (size_t i = 0; i < ctx.count; i++)
		_nuttx_list_finish(&out[i], &g_nx_list_snapshot[i], dt_us);

#if !defined(CONFIG_ARCH_PERF_EVENTS)
	for (int k = 0; k < OVE_NX_CPUINT_MAX; k++)
		if (g_nx_cpuint[k].used && !g_nx_cpuint[k].seen)
			g_nx_cpuint[k].used = false;
#endif

	if (actual_count)
		*actual_count = ctx.count;
	nxmutex_unlock(&g_nx_list_lock);
	return (ctx.overflow
#if defined(CONFIG_ARCH_PERF_EVENTS)
		|| g_nx_runtime_overflow
#else
		|| g_nx_cpuint_overflow
#endif
		)
		       ? OVE_ERR_QUEUE_FULL
		       : OVE_OK;
}
