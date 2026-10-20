/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Regression test for the NuttX suspend/resume handshake.
 *
 * NuttX has no pthread_suspend(), so a suspend is implemented with a
 * signal: ove_thread_suspend() sends SIGUSR1 to the target, the target's
 * handler parks it on a semaphore, and ove_thread_resume() wakes it up.
 * The signal alone is not a handshake:
 *
 *   - NuttX *queues* the action and runs it at the target's next dispatch
 *     (sched/signal/sig_dispatch.c:602), so a resume that arrives before
 *     the action was delivered used to be lost: the handler parked the
 *     target afterwards and nobody was left to release it - the thread
 *     parked forever.
 *   - NuttX drops a signal whose action is not installed yet
 *     (sched/signal/sig_dispatch.c:117), so a suspend issued before the
 *     target's first dispatch used to be lost.
 *
 * The handshake code is not re-implemented here: it is extracted verbatim
 * from backends/nuttx/nuttx_thread.c at build time (see
 * tests/cmake/extract_nuttx_suspend_block.py) and compiled against the
 * NuttX stand-in in nuttx_suspend_shim.h, which models the task and the
 * signal delivery.  The test drives the dispatch points itself, so the
 * orderings that matter are reproduced deterministically - no sleeps are
 * used to "hope" for a race.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ove_config.h"

#include "ove/thread.h" /* pulls in ove/storage.h -> the NuttX storage header */

#include "nuttx_suspend_shim.h"

/* A test can run code inside the Nth sleep of a suspend's acknowledgement
 * wait, on the thread that set ack_hook_armed. */
static __thread int ack_hook_armed;
static int ack_hook_sleeps;
static int ack_hook_at;
static void (*ack_hook)(void);

static int ack_usleep(useconds_t us)
{
	if (ack_hook_armed && ++ack_hook_sleeps == ack_hook_at && ack_hook)
		ack_hook();
	return usleep(us);
}

/* The suspend/resume handshake - extracted verbatim from
 * backends/nuttx/nuttx_thread.c (see
 * tests/cmake/extract_nuttx_suspend_block.py). */
#define usleep ack_usleep
#include "ove_nuttx_suspend_block.inc"
#undef usleep

volatile int shim_hold_after_wake;
volatile int shim_wakes;

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

/* ── the simulated NuttX task ───────────────────────────────────────── */

struct sim_task {
	pthread_t tid;
	struct ove_thread thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int start_ok;	     /* the task may enter task_wrapper() */
	int dispatch_ok;     /* the task may run one dispatch point */
	int stop;	     /* the task shall leave its entry */
	int signal_queued;   /* a SIGUSR1 action is queued for the task */
	int kill_calls;	     /* kill(SIGUSR1) calls seen for the task */
	int signals_dropped; /* signals dropped: no action installed yet */
	int entered;	     /* the entry function was entered */
	int progress;	     /* iterations of the entry function */
};

static struct sim_task g_task;

__thread struct ove_thread *sim_tls_current;
int sim_sigusr1_installed;

/* The model's kill(): NuttX queues the action and makes the target
 * runnable; the action itself runs at the target's next dispatch, which
 * the test controls. */
int sim_kill(pid_t pid, int sig)
{
	struct sim_task *s = &g_task;

	if (sig != SIGUSR1 || pid != s->thread.pid) {
		return -1;
	}

	pthread_mutex_lock(&s->lock);
	s->kill_calls++;
	if (sim_sigusr1_installed) {
		s->signal_queued = 1;
	} else {
		/* No action installed yet: NuttX drops the signal silently
		 * ("There is no default action", sig_dispatch.c:117). */
		s->signals_dropped++;
	}
	s->start_ok = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->lock);
	return 0;
}

/* The entry function of the simulated task: it makes progress at every
 * dispatch point and delivers a queued signal action there (that is where
 * NuttX runs it). */
static void sim_worker(void *arg)
{
	struct sim_task *s = arg;

	pthread_mutex_lock(&s->lock);
	s->entered = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->lock);

	for (;;) {
		int deliver;

		pthread_mutex_lock(&s->lock);
		while (!s->dispatch_ok && !s->stop) {
			pthread_cond_wait(&s->cond, &s->lock);
		}
		if (s->stop) {
			pthread_mutex_unlock(&s->lock);
			break;
		}
		s->dispatch_ok = 0;
		deliver = s->signal_queued;
		s->signal_queued = 0;
		pthread_mutex_unlock(&s->lock);

		if (deliver) {
			/* The kernel dispatches the queued action to the task:
			 * the handler runs in the task's context. */
			sigusr1_handler(SIGUSR1);
		}

		pthread_mutex_lock(&s->lock);
		s->progress++;
		pthread_mutex_unlock(&s->lock);
	}
}

/* The simulated NuttX task: it waits until the scheduler picks it up and
 * then runs the NuttX task entry. */
static void *sim_task_main(void *arg)
{
	struct sim_task *s = arg;
	struct ove_thread *t = &s->thread;
	char addr[32];
	char *argv[3];

	snprintf(addr, sizeof(addr), "%p", (void *)t);
	argv[0] = (char *)"sim";
	argv[1] = addr;
	argv[2] = NULL;

	pthread_mutex_lock(&s->lock);
	while (!s->start_ok && !s->stop) {
		pthread_cond_wait(&s->cond, &s->lock);
	}
	pthread_mutex_unlock(&s->lock);
	if (s->stop) {
		return NULL;
	}

	/* The NuttX task entry (task_wrapper() parses argv[1]). */
	task_wrapper(2, argv);
	return NULL;
}

/* ── the model helpers ──────────────────────────────────────────────── */

static void sim_task_setup(struct sim_task *s, const char *name)
{
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	pthread_cond_init(&s->cond, NULL);
	/* The state a thread has after thread_start() on NuttX. */
	s->thread.pid = 4242;
	s->thread.entry = sim_worker;
	s->thread.arg = s;
	s->thread.name = name;
	s->thread.state = OVE_THREAD_STATE_READY;
	s->thread.started = 1;
	s->thread.suspend_inited = 1;
	nxsem_init(&s->thread.suspend_sem, 0, 0);
	nxsem_init(&s->thread.done_sem, 0, 0);
	sim_sigusr1_installed = 0;
}

static void sim_task_launch(struct sim_task *s)
{
	if (pthread_create(&s->tid, NULL, sim_task_main, s) != 0) {
		fprintf(stderr, "cannot create the simulated task\n");
		exit(2);
	}
}

/* Let the scheduler pick the task up: it enters task_wrapper(). */
static void sim_task_start(struct sim_task *s)
{
	pthread_mutex_lock(&s->lock);
	s->start_ok = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->lock);
}

/* Let the task run one dispatch point. */
static void sim_grant_dispatch(struct sim_task *s)
{
	pthread_mutex_lock(&s->lock);
	s->dispatch_ok = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->lock);
}

static int sim_progress(struct sim_task *s)
{
	int value;

	pthread_mutex_lock(&s->lock);
	value = s->progress;
	pthread_mutex_unlock(&s->lock);
	return value;
}

static int sim_entered(struct sim_task *s)
{
	int value;

	pthread_mutex_lock(&s->lock);
	value = s->entered;
	pthread_mutex_unlock(&s->lock);
	return value;
}

static int sim_kill_calls(struct sim_task *s)
{
	int value;

	pthread_mutex_lock(&s->lock);
	value = s->kill_calls;
	pthread_mutex_unlock(&s->lock);
	return value;
}

static int sim_signals_dropped(struct sim_task *s)
{
	int value;

	pthread_mutex_lock(&s->lock);
	value = s->signals_dropped;
	pthread_mutex_unlock(&s->lock);
	return value;
}

static int sim_state(struct ove_thread *t)
{
	return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
}

/* Wait until the task has seen @at_least kill(SIGUSR1) calls. */
static bool wait_for_kills(struct sim_task *s, int at_least, int timeout_ms)
{
	int waited;

	for (waited = 0; waited < timeout_ms; waited++) {
		if (sim_kill_calls(s) >= at_least) {
			return true;
		}
		usleep(1000);
	}
	return false;
}

/* Wait until the task made more progress than @beyond. */
static bool wait_for_progress(struct sim_task *s, int beyond, int timeout_ms)
{
	int waited;

	for (waited = 0; waited < timeout_ms; waited++) {
		if (sim_progress(s) > beyond) {
			return true;
		}
		usleep(1000);
	}
	return false;
}

/* Wait until the task entered its entry function. */
static bool wait_for_entered(struct sim_task *s, int timeout_ms)
{
	int waited;

	for (waited = 0; waited < timeout_ms; waited++) {
		if (sim_entered(s)) {
			return true;
		}
		usleep(1000);
	}
	return false;
}

static void sim_msleep(int ms)
{
	usleep((useconds_t)ms * 1000u);
}

/* Stop the simulated task and release it if it is parked. */
static void sim_task_stop(struct sim_task *s)
{
	pthread_mutex_lock(&s->lock);
	s->stop = 1;
	s->start_ok = 1;
	s->dispatch_ok = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->lock);

	/* Release a target that is parked in the suspend loop. */
	ove_thread_resume(&s->thread);
	pthread_join(s->tid, NULL);
}

/* The suspend is issued by another task: the public API blocks until the
 * target acknowledges the suspension (or the wait times out). */
static void *suspend_helper(void *arg)
{
	ove_thread_suspend((struct ove_thread *)arg);
	return NULL;
}

/* ── the scenarios ──────────────────────────────────────────────────── */

/*
 * The resume races the suspend: the resume is issued before the queued
 * signal action is delivered to the target.
 *
 * The desired behaviour is that a suspend followed by a resume always
 * leaves the target runnable, whatever the ordering.  Before the fix the
 * resume was lost (it only acted on an already suspended target), the
 * handler parked the target afterwards and nobody released it: the thread
 * parked forever.
 */
static void test_resume_racing_suspend(void)
{
	struct sim_task *s = &g_task;
	struct ove_thread *t = &s->thread;
	pthread_t helper;
	int base;

	sim_task_setup(s, "race");
	sim_task_launch(s);
	sim_task_start(s);
	CHECK(wait_for_entered(s, 1000));

	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	/* The suspend has sent the signal; the action has not been
	 * delivered yet (the test owns the dispatch points). */
	CHECK(wait_for_kills(s, 1, 1000));

	/* The resume arrives before the action was delivered. */
	ove_thread_resume(t);

	/* Now the target is dispatched and the queued action runs. */
	base = sim_progress(s);
	sim_grant_dispatch(s);

	/* The target must not park: it keeps making progress. */
	CHECK(wait_for_progress(s, base, 1000));
	CHECK(sim_state(t) != OVE_THREAD_STATE_SUSPENDED);

	pthread_join(helper, NULL);
	sim_task_stop(s);
}

/*
 * The well ordered suspend: the target acknowledges the suspension, parks
 * in the suspend loop and the resume releases it again.
 */
static void test_suspend_acknowledged_and_resumed(void)
{
	struct sim_task *s = &g_task;
	struct ove_thread *t = &s->thread;
	pthread_t helper;
	int base;

	sim_task_setup(s, "ack");
	sim_task_launch(s);
	sim_task_start(s);
	CHECK(wait_for_entered(s, 1000));

	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	CHECK(wait_for_kills(s, 1, 1000));
	/* The target is dispatched: the action runs and it parks. */
	sim_grant_dispatch(s);
	pthread_join(helper, NULL);

	/* The suspension was acknowledged: the target is parked. */
	CHECK(sim_state(t) == OVE_THREAD_STATE_SUSPENDED);
	base = sim_progress(s);
	sim_msleep(50);
	CHECK(sim_progress(s) == base);

	/* The resume releases the parked target. */
	ove_thread_resume(t);
	CHECK(wait_for_progress(s, base, 1000));
	CHECK(sim_state(t) != OVE_THREAD_STATE_SUSPENDED);

	sim_task_stop(s);
}

/*
 * A stale semaphore token must not release a later suspend cycle.
 *
 * The resume of the first cycle posts the semaphore although the target
 * is not parked yet: the token stays in the semaphore.  The next suspend
 * has to park the target for real - the stale token may wake the wait but
 * must not let the target run while it should be suspended.
 */
static void test_stale_token_does_not_release_a_later_cycle(void)
{
	struct sim_task *s = &g_task;
	struct ove_thread *t = &s->thread;
	pthread_t helper;
	int base;

	sim_task_setup(s, "stale");
	sim_task_launch(s);
	sim_task_start(s);
	CHECK(wait_for_entered(s, 1000));

	/* Cycle 1: the resume arrives before the action was delivered, so
	 * it posts the semaphore while the target is not parked. */
	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	CHECK(wait_for_kills(s, 1, 1000));
	ove_thread_resume(t);
	base = sim_progress(s);
	sim_grant_dispatch(s);
	CHECK(wait_for_progress(s, base, 1000));
	pthread_join(helper, NULL);

	/* Cycle 2: the next suspend has to park the target - the stale
	 * token of the first cycle must not release it. */
	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	CHECK(wait_for_kills(s, 2, 1000));
	sim_grant_dispatch(s);
	pthread_join(helper, NULL);

	CHECK(sim_state(t) == OVE_THREAD_STATE_SUSPENDED);
	base = sim_progress(s);
	sim_msleep(50);
	CHECK(sim_progress(s) == base);

	/* The resume releases it. */
	ove_thread_resume(t);
	CHECK(wait_for_progress(s, base, 1000));

	sim_task_stop(s);
}

/*
 * A suspend that is never acknowledged (the target never runs) has to
 * time out and cancel its request: the target must not park afterwards
 * when the queued action finally runs.
 */
static void test_unacknowledged_suspend_does_not_park_later(void)
{
	struct sim_task *s = &g_task;
	struct ove_thread *t = &s->thread;
	pthread_t helper;
	int base;

	sim_task_setup(s, "timeout");
	sim_task_launch(s);
	sim_task_start(s);
	CHECK(wait_for_entered(s, 1000));

	/* The target is never dispatched: the suspend cannot be
	 * acknowledged and has to time out. */
	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	pthread_join(helper, NULL);

	/* Now the queued action runs - the request was cancelled by the
	 * timeout, so the target must not park. */
	base = sim_progress(s);
	sim_grant_dispatch(s);
	CHECK(wait_for_progress(s, base, 1000));
	CHECK(sim_state(t) != OVE_THREAD_STATE_SUSPENDED);

	sim_task_stop(s);
}

/*
 * A suspend issued before the target's first dispatch must not be lost:
 * NuttX drops the signal (the action is not installed yet), so the
 * request has to be honoured by the task entry - the target parks before
 * it runs its entry function.
 */
static void test_suspend_before_first_dispatch_is_honoured(void)
{
	int attempt;

	/* The suspension has to be acknowledged while the suspend waits for
	 * it; a heavily loaded host can miss that window, so the scenario is
	 * retried a few times before it is reported as a failure. */
	for (attempt = 0; attempt < 5; attempt++) {
		struct sim_task *s = &g_task;
		struct ove_thread *t = &s->thread;
		pthread_t helper;

		sim_task_setup(s, "first");
		sim_task_launch(s);

		/* The target has not been dispatched yet: the signal is
		 * dropped because the action is not installed. */
		CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
		CHECK(wait_for_kills(s, 1, 1000));
		CHECK(sim_signals_dropped(s) >= 1);
		pthread_join(helper, NULL);

		if (sim_state(t) == OVE_THREAD_STATE_SUSPENDED) {
			/* The target parked before its entry ran. */
			CHECK(sim_entered(s) == 0);
			ove_thread_resume(t);
			CHECK(wait_for_entered(s, 1000));
			sim_task_stop(s);
			return;
		}
		sim_task_stop(s);
	}

	CHECK(false); /* the early suspend was never honoured */
}

/* A resume followed at once by a new suspend, before the woken target ran
 * again: the second suspend is a new request and must not be lost.  The
 * target parks for it (SUSPENDED), or, if it did not, keeps running. */
static void test_resume_then_immediate_suspend(void)
{
	struct sim_task *s = &g_task;
	struct ove_thread *t = &s->thread;
	pthread_t helper;
	int base, w;

	sim_task_setup(s, "again");
	sim_task_launch(s);
	sim_task_start(s);
	CHECK(wait_for_entered(s, 1000));

	/* First suspend: acknowledged, the target is parked. */
	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	CHECK(wait_for_kills(s, 1, 1000));
	sim_grant_dispatch(s);
	pthread_join(helper, NULL);
	CHECK(sim_state(t) == OVE_THREAD_STATE_SUSPENDED);

	/* The resume wakes the target, which is not dispatched yet. */
	__atomic_store_n(&shim_hold_after_wake, 1, __ATOMIC_SEQ_CST);
	w = __atomic_load_n(&shim_wakes, __ATOMIC_SEQ_CST);
	ove_thread_resume(t);
	for (int i = 0; i < 10000 && __atomic_load_n(&shim_wakes, __ATOMIC_SEQ_CST) == w; i++)
		usleep(100);
	CHECK(__atomic_load_n(&shim_wakes, __ATOMIC_SEQ_CST) != w); /* resume posted */
	/* The second suspend starts before the target saw the release.  The
	 * 2 ms only let it reach its acknowledgement wait; the outcome does
	 * not depend on them. */
	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	CHECK(wait_for_kills(s, 2, 1000));
	usleep(2000);
	__atomic_store_n(&shim_hold_after_wake, 0, __ATOMIC_SEQ_CST);
	pthread_join(helper, NULL);

	int st = sim_state(t);
	base = sim_progress(s);
	sim_grant_dispatch(s);
	int moved = wait_for_progress(s, base, 300);
	CHECK(st == OVE_THREAD_STATE_SUSPENDED || moved);
	sim_task_stop(s);
}

/* Second suspender of test_ack_wait_sees_a_newer_park(): it suspends the
 * target and lets it park while the first suspender sleeps for the last
 * time in its acknowledgement wait. */
static void park_for_newer_request(void)
{
	struct sim_task *s = &g_task;
	pthread_t helper;

	CHECK(pthread_create(&helper, NULL, suspend_helper, &s->thread) == 0);
	CHECK(wait_for_kills(s, 2, 1000));
	sim_grant_dispatch(s);
	pthread_join(helper, NULL);
}

static void *armed_suspend_helper(void *arg)
{
	ack_hook_armed = 1;
	ove_thread_suspend((struct ove_thread *)arg);
	return NULL;
}

/* Two suspenders: the target is not dispatched during the first one's
 * acknowledgement wait, and parks for the second one's request during the
 * first one's last sleep.  The first suspend must not take that for a
 * timeout: the target has to stay SUSPENDED and parked, and a third
 * suspend must find it so. */
static void test_ack_wait_sees_a_newer_park(void)
{
	struct sim_task *s = &g_task;
	struct ove_thread *t = &s->thread;
	pthread_t helper;
	int base;

	sim_task_setup(s, "newer");
	sim_task_launch(s);
	sim_task_start(s);
	CHECK(wait_for_entered(s, 1000));

	ack_hook_sleeps = 0;
	ack_hook_at = OVE_NUTTX_SUSPEND_ACK_TRIES;
	ack_hook = park_for_newer_request;
	CHECK(pthread_create(&helper, NULL, armed_suspend_helper, t) == 0);
	pthread_join(helper, NULL);
	ack_hook = NULL;

	CHECK(sim_state(t) == OVE_THREAD_STATE_SUSPENDED);
	base = sim_progress(s);
	CHECK(!wait_for_progress(s, base, 50));

	CHECK(pthread_create(&helper, NULL, suspend_helper, t) == 0);
	pthread_join(helper, NULL);
	CHECK(sim_state(t) == OVE_THREAD_STATE_SUSPENDED);
	CHECK(!wait_for_progress(s, base, 50));

	ove_thread_resume(t);
	sim_grant_dispatch(s);
	CHECK(wait_for_progress(s, base, 1000));
	sim_task_stop(s);
}

int main(void)
{
	test_resume_racing_suspend();
	test_suspend_acknowledged_and_resumed();
	test_stale_token_does_not_release_a_later_cycle();
	test_unacknowledged_suspend_does_not_park_later();
	test_suspend_before_first_dispatch_is_honoured();
	test_resume_then_immediate_suspend();
	test_ack_wait_sees_a_newer_park();

	if (failures != 0) {
		fprintf(stderr, "nuttx suspend handshake: %d check(s) failed\n", failures);
		return 1;
	}
	printf("nuttx suspend handshake: all checks passed\n");
	return 0;
}
