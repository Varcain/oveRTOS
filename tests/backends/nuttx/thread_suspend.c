/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "framework/ove_test.h"
#include <pthread.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include "ove_thread_registry.h"
#include <stdatomic.h>
#include <unistd.h>

void ove_test_nuttx_thread_suspend(ove_thread_storage_t *storage, void *stack, size_t size,
				   struct ove_thread_info *info, size_t capacity);
int ove_test_nuttx_sigaction(int sig, const struct sigaction *action, struct sigaction *old);
int ove_test_nuttx_tgkill(pid_t group, pid_t tid, int sig);
int ove_test_nuttx_suspend_wait(sem_t *sem);
_Noreturn void ove_test_nuttx_suspend_exit(int status);

static void suspend_protocol_tests(ove_thread_storage_t *storage, void *stack, size_t size,
				   struct ove_thread_info *info, size_t capacity);

struct suspend_probe {
	atomic_int owner_count;
	atomic_int sibling_count;
	atomic_int ready;
	atomic_int stop;
	ove_thread_t sibling_self;
	int sibling_result;
};

static void *suspend_sibling(void *arg)
{
	struct suspend_probe *p = arg;
	p->sibling_self = ove_thread_get_self();
	while (!atomic_load(&p->stop)) {
		atomic_fetch_add(&p->sibling_count, 1);
		usleep(1000);
	}
	return NULL;
}

static void suspend_owner(void *arg)
{
	struct suspend_probe *p = arg;
	pthread_t sibling;
	p->sibling_result = pthread_create(&sibling, NULL, suspend_sibling, p);
	atomic_store(&p->ready, 1);
	while (!atomic_load(&p->stop)) {
		atomic_fetch_add(&p->owner_count, 1);
		usleep(1000);
	}
	if (!p->sibling_result)
		(void)pthread_join(sibling, NULL);
}

void ove_test_nuttx_thread_suspend(ove_thread_storage_t *storage, void *stack, size_t size,
				   struct ove_thread_info *info, size_t capacity)
{
	struct suspend_probe p = {0};
	ove_thread_t target = NULL;
	int created = ove_thread_init(&target, storage, "suspend-owner", suspend_owner, &p,
				      OVE_PRIO_NORMAL, size, stack);
	assert_int_equal(created, OVE_OK);
	for (unsigned i = 0; i < 1000 && !atomic_load(&p.ready); ++i)
		usleep(1000);
	usleep(30000);
	ove_thread_suspend(target);
	int owner_before = atomic_load(&p.owner_count);
	int sibling_before = atomic_load(&p.sibling_count);
	ove_thread_state_t parked = ove_thread_get_state(target);
	usleep(30000);
	int owner_after = atomic_load(&p.owner_count);
	int sibling_after = atomic_load(&p.sibling_count);
	ove_thread_resume(target);
	usleep(30000);
	int owner_resumed = atomic_load(&p.owner_count);
	atomic_store(&p.stop, 1);
	int joined = ove_thread_deinit(target);
	assert_int_equal(joined, OVE_OK);
	assert_true(atomic_load(&p.ready));
	assert_int_equal(p.sibling_result, 0);
	assert_null(p.sibling_self);
	assert_true(owner_before > 0);
	assert_int_equal(owner_after, owner_before);
	assert_true(sibling_after > sibling_before);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_true(owner_resumed > owner_after);
	suspend_protocol_tests(storage, stack, size, info, capacity);
}

/* Native seams are restricted to the backend translation unit. Each gate is
 * retained until all actors that may access it have returned. */
struct protocol_probe {
	ove_thread_t target;
	pid_t creator;
	pid_t setup_pid;
	atomic_int ready;
	atomic_int stop;
	atomic_int masked;
	atomic_int unmask;
	atomic_int self_park;
	atomic_int after_self;
	atomic_int hold_registry;
	atomic_int hold_send;
	atomic_int send_held;
	atomic_int send_error;
	atomic_int send_count;
	atomic_int fail_setup;
	atomic_int hold_setup;
	atomic_int setup_held;
	atomic_int hold_exit;
	atomic_int exit_held;
	atomic_int hold_wait;
	atomic_int wait_held;
	atomic_int progress;
	atomic_int entry_returned;
	atomic_int block_owner;
	ove_sem_storage_t block_storage;
	ove_sem_t block_sem;
	int block_result;
	sem_t send_gate;
	sem_t wait_gate;
	sem_t exit_gate;
	sem_t setup_gate;
	pthread_t actors[2];
	unsigned int actor_count;
	int target_started;
};

struct protocol_caller {
	struct protocol_probe *probe;
	atomic_int started;
	atomic_int done;
	int close;
	int create;
	void *stack;
	size_t size;
	_Atomic(ove_thread_t) output;
	int result;
};

static _Atomic(struct protocol_probe *) protocol_active;

static int protocol_wait(atomic_int *value, int expected)
{
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	uint64_t deadline = now + 2000000;
	while (atomic_load(value) != expected) {
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return 0;
		usleep(1000);
	}
	return 1;
}

int ove_test_nuttx_sigaction(int sig, const struct sigaction *action, struct sigaction *old)
{
	irqstate_t flags = enter_critical_section();
	struct protocol_probe *p = atomic_load(&protocol_active);
	int selected = p && sig == SIGUSR1 && action && getpid() != p->creator &&
		       ove_thread_get_self() == p->target;
	int hold = selected && atomic_exchange(&p->hold_setup, 0);
	if (hold)
		atomic_store(&p->setup_held, 1);
	leave_critical_section(flags);
	if (hold) {
		(void)nxsem_wait_uninterruptible(&p->setup_gate);
		atomic_store(&p->setup_held, 0);
	}
	if (selected) {
		flags = enter_critical_section();
		int fail = atomic_exchange(&p->fail_setup, 0);
		if (fail)
			p->setup_pid = getpid();
		leave_critical_section(flags);
		if (fail) {
			errno = ENOMEM;
			return -1;
		}
	}
	return sigaction(sig, action, old);
}

int ove_test_nuttx_tgkill(pid_t group, pid_t tid, int sig)
{
	irqstate_t flags = enter_critical_section();
	struct protocol_probe *p = atomic_load(&protocol_active);
	int selected = p && sig == SIGUSR1 && tid == p->target->pid;
	int hold = 0;
	int error = 0;
	if (selected) {
		atomic_fetch_add(&p->send_count, 1);
		hold = atomic_exchange(&p->hold_send, 0);
		error = atomic_load(&p->send_error);
		if (hold)
			atomic_store(&p->send_held, 1);
	}
	leave_critical_section(flags);
	if (hold) {
		(void)nxsem_wait_uninterruptible(&p->send_gate);
		atomic_store(&p->send_held, 0);
		if (error)
			return -EAGAIN;
	}
	if (selected && error == 2)
		return -EAGAIN;
	return nxsig_tgkill(group, tid, sig);
}

int ove_test_nuttx_suspend_wait(sem_t *sem)
{
	irqstate_t flags = enter_critical_section();
	struct protocol_probe *p = atomic_load(&protocol_active);
	int hold = p && sem == &p->target->suspend_sem && atomic_exchange(&p->hold_wait, 0);
	if (hold)
		atomic_store(&p->wait_held, 1);
	leave_critical_section(flags);
	if (hold) {
		(void)nxsem_wait_uninterruptible(&p->wait_gate);
		atomic_store(&p->wait_held, 0);
	}
	return nxsem_wait_uninterruptible(sem);
}

_Noreturn void ove_test_nuttx_suspend_exit(int status)
{
	irqstate_t flags = enter_critical_section();
	struct protocol_probe *p = atomic_load(&protocol_active);
	int hold = p && getpid() == p->setup_pid && atomic_load(&p->hold_exit);
	if (hold)
		atomic_store(&p->exit_held, 1);
	leave_critical_section(flags);
	if (hold) {
		(void)nxsem_wait_uninterruptible(&p->exit_gate);
		atomic_store(&p->exit_held, 0);
	}
	exit(status);
}

static void protocol_owner(void *arg)
{
	struct protocol_probe *p = arg;
	if (atomic_load(&p->masked)) {
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGUSR1);
		(void)sigprocmask(SIG_BLOCK, &mask, NULL);
	}
	atomic_store(&p->ready, 1);
	if (atomic_load(&p->self_park)) {
		ove_thread_suspend(ove_thread_get_self());
		atomic_store(&p->after_self, 1);
	}
	if (atomic_load(&p->block_owner))
		p->block_result = ove_sem_take(p->block_sem, OVE_WAIT_FOREVER);
	while (!atomic_load(&p->stop) && !ove_thread_should_stop(ove_thread_get_self())) {
		if (atomic_load(&p->hold_registry) == 1) {
			ove_backend_thread_registry_lock();
			atomic_store(&p->hold_registry, 2);
		}
		if (atomic_load(&p->masked) && atomic_load(&p->unmask)) {
			sigset_t mask;
			sigemptyset(&mask);
			sigaddset(&mask, SIGUSR1);
			(void)sigprocmask(SIG_UNBLOCK, &mask, NULL);
			atomic_store(&p->masked, 0);
		}
		atomic_fetch_add(&p->progress, 1);
		usleep(1000);
	}
	if (atomic_load(&p->hold_registry) == 2)
		ove_backend_thread_registry_unlock();
	atomic_store(&p->entry_returned, 1);
}

static void *protocol_call(void *arg)
{
	struct protocol_caller *c = arg;
	atomic_store(&c->started, 1);
	if (c->create) {
		ove_thread_t output = atomic_load(&c->output);
		c->result = ove_thread_init(&output, c->probe->target, "setup-fail", protocol_owner,
					    c->probe, OVE_PRIO_HIGH, c->size, c->stack);
		atomic_store(&c->output, output);
		if (c->result == OVE_OK)
			c->probe->target_started = 1;
	} else if (c->close)
		c->result = ove_thread_deinit(c->probe->target);
	else
		ove_thread_suspend(c->probe->target);
	atomic_store(&c->done, 1);
	return NULL;
}

static void protocol_init(struct protocol_probe *p, ove_thread_storage_t *storage)
{
	static unsigned int scenario;
	print_message("  native suspension scenario %u\n", ++scenario);
	*p = (struct protocol_probe){.target = storage, .creator = getpid()};
	assert_int_equal(nxsem_init(&p->send_gate, 0, 0), 0);
	assert_int_equal(nxsem_set_protocol(&p->send_gate, SEM_PRIO_NONE), 0);
	assert_int_equal(nxsem_init(&p->wait_gate, 0, 0), 0);
	assert_int_equal(nxsem_set_protocol(&p->wait_gate, SEM_PRIO_NONE), 0);
	assert_int_equal(nxsem_init(&p->setup_gate, 0, 0), 0);
	assert_int_equal(nxsem_set_protocol(&p->setup_gate, SEM_PRIO_NONE), 0);
	assert_int_equal(nxsem_init(&p->exit_gate, 0, 0), 0);
	assert_int_equal(nxsem_set_protocol(&p->exit_gate, SEM_PRIO_NONE), 0);
	OVE_TEST_ASSERT_OK(ove_sem_init(&p->block_sem, &p->block_storage, 0, 1));
	atomic_store(&protocol_active, p);
}

static int protocol_start(struct protocol_probe *p, void *stack, size_t size)
{
	ove_thread_t result = NULL;
	int rc = ove_thread_init(&result, p->target, "protocol-owner", protocol_owner, p,
				 OVE_PRIO_HIGH, size, stack);
	if (rc != OVE_OK)
		return 0;
	p->target_started = 1;
	return protocol_wait(&p->ready, 1);
}

static int protocol_finish(struct protocol_probe *p)
{
	atomic_store(&p->stop, 1);
	atomic_store(&p->unmask, 1);
	(void)nxsem_post(&p->send_gate);
	(void)nxsem_post(&p->wait_gate);
	(void)nxsem_post(&p->exit_gate);
	(void)nxsem_post(&p->setup_gate);
	ove_thread_resume(p->target);
	ove_sem_give(p->block_sem);
	int rc = ove_thread_deinit(p->target);
	{
		irqstate_t flags = enter_critical_section();
		atomic_store(&protocol_active, NULL);
		leave_critical_section(flags);
	}
	ove_sem_deinit(p->block_sem);
	(void)nxsem_destroy(&p->send_gate);
	(void)nxsem_destroy(&p->wait_gate);
	(void)nxsem_destroy(&p->exit_gate);
	(void)nxsem_destroy(&p->setup_gate);
	p->target_started = 0;
	return rc;
}

static void protocol_join(struct protocol_probe *p, pthread_t actor)
{
	int rc = pthread_join(actor, NULL);
	if (rc != 0) {
		/* Keep caller frames/gates alive until the external runner deadline. */
		print_error("native caller join failed: %d\n", rc);
		for (;;)
			usleep(1000);
	}
	for (unsigned int i = 0; i < p->actor_count; ++i)
		if (pthread_equal(p->actors[i], actor)) {
			p->actors[i] = p->actors[--p->actor_count];
			break;
		}
}

static void protocol_spawn(struct protocol_probe *p, pthread_t *actor, struct protocol_caller *c)
{
	int rc = pthread_create(actor, NULL, protocol_call, c);
	if (rc == 0) {
		p->actors[p->actor_count++] = *actor;
		return;
	}
	/* Release all owned gates before joining any previously started caller. */
	atomic_store(&p->stop, 1);
	atomic_store(&p->unmask, 1);
	(void)nxsem_post(&p->send_gate);
	(void)nxsem_post(&p->wait_gate);
	(void)nxsem_post(&p->exit_gate);
	(void)nxsem_post(&p->setup_gate);
	if (p->target_started) {
		int joined = protocol_finish(p);
		(void)joined;
	}
	while (p->actor_count)
		protocol_join(p, p->actors[p->actor_count - 1]);
	{
		irqstate_t flags = enter_critical_section();
		atomic_store(&protocol_active, NULL);
		leave_critical_section(flags);
	}
	fail_msg("native caller creation failed: %d", rc);
}

static void protocol_reap(pid_t pid)
{
	for (;;) {
		irqstate_t flags = enter_critical_section();
		int gone = pid > 0 && nxsched_get_tcb(pid) == NULL;
		leave_critical_section(flags);
		if (gone)
			return;
		/* Do not discard backing if a broken implementation exits late. */
		usleep(1000);
	}
}

static int protocol_pending(struct protocol_probe *p)
{
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	uint64_t deadline = now + 2000000;
	for (;;) {
		irqstate_t flags = enter_critical_section();
		int pending = p->target->suspend_active && !p->target->suspend_acked;
		leave_critical_section(flags);
		if (pending)
			return 1;
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return 0;
		usleep(1000);
	}
}

static int protocol_phase(struct protocol_probe *p, int users, int native_wait)
{
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	uint64_t deadline = now + 2000000;
	for (;;) {
		irqstate_t flags = enter_critical_section();
		struct tcb_s *native = nxsched_get_tcb(p->target->pid);
		int ready = native_wait ? native && native->task_state == TSTATE_WAIT_SEM
					: p->target->suspend_users >= (unsigned int)users;
		leave_critical_section(flags);
		if (ready)
			return 1;
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return 0;
		usleep(1000);
	}
}

static void protocol_creator_handler(int sig)
{
	(void)sig;
}

static void suspend_protocol_tests(ove_thread_storage_t *storage, void *stack, size_t size,
				   struct ove_thread_info *info, size_t capacity)
{
	struct protocol_probe p;
	struct protocol_caller a = {0}, b = {0};
	pthread_t ta, tb;
	/* Readiness precedes publication even when the first entry self-parks. */
	protocol_init(&p, storage);
	atomic_store(&p.self_park, 1);
	int started = protocol_start(&p, stack, size);
	int before = atomic_load(&p.after_self);
	ove_thread_resume(p.target);
	int after = protocol_wait(&p.after_self, 1);
	int joined = protocol_finish(&p);
	assert_true(started);
	assert_int_equal(before, 0);
	assert_true(after);
	assert_int_equal(joined, OVE_OK);

	/* A creator's distinct action and inherited blocked mask survive setup. */
	struct sigaction own = {0}, saved, observed;
	sigset_t mask, saved_mask, observed_mask;
	own.sa_handler = protocol_creator_handler;
	sigemptyset(&own.sa_mask);
	(void)sigaction(SIGUSR1, &own, &saved);
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	(void)sigprocmask(SIG_BLOCK, &mask, &saved_mask);
	protocol_init(&p, storage);
	started = protocol_start(&p, stack, size);
	ove_thread_suspend(p.target);
	int parked = ove_thread_get_state(p.target);
	(void)sigaction(SIGUSR1, NULL, &observed);
	(void)sigprocmask(SIG_SETMASK, NULL, &observed_mask);
	joined = protocol_finish(&p);
	(void)sigaction(SIGUSR1, &saved, NULL);
	(void)sigprocmask(SIG_SETMASK, &saved_mask, NULL);
	assert_true(started);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_true(observed.sa_handler == own.sa_handler);
	assert_int_equal(sigismember(&observed_mask, SIGUSR1), 1);
	assert_int_equal(joined, OVE_OK);

	/* Creation cannot publish while its target setup is held incomplete. */
	protocol_init(&p, storage);
	atomic_store(&p.hold_setup, 1);
	a = (struct protocol_caller){.probe = &p,
				     .create = 1,
				     .stack = stack,
				     .size = size,
				     .output = (ove_thread_t)(uintptr_t)1};
	protocol_spawn(&p, &ta, &a);
	int setup_held = protocol_wait(&p.setup_held, 1);
	usleep(20000);
	int not_published = !atomic_load(&a.done) && a.output == (ove_thread_t)(uintptr_t)1;
	(void)nxsem_post(&p.setup_gate);
	int create_completed = protocol_wait(&a.done, 1);
	protocol_join(&p, ta);
	joined = protocol_finish(&p);
	assert_true(setup_held && not_published && create_completed);
	assert_int_equal(a.result, OVE_OK);
	assert_true(a.output == storage);
	assert_int_equal(joined, OVE_OK);

	/* Reported setup failure leaves output untouched and backing reusable. */
	protocol_init(&p, storage);
	atomic_store(&p.fail_setup, 1);
	atomic_store(&p.hold_exit, 1);
	a = (struct protocol_caller){.probe = &p,
				     .create = 1,
				     .stack = stack,
				     .size = size,
				     .output = (ove_thread_t)(uintptr_t)1};
	protocol_spawn(&p, &ta, &a);
	int exit_held = protocol_wait(&p.exit_held, 1);
	usleep(20000);
	int setup_waited = !atomic_load(&a.done);
	(void)nxsem_post(&p.exit_gate);
	int setup_completed = protocol_wait(&a.done, 1);
	protocol_join(&p, ta);
	irqstate_t flags = enter_critical_section();
	int gone = p.setup_pid > 0 && nxsched_get_tcb(p.setup_pid) == NULL;
	leave_critical_section(flags);
	protocol_reap(p.setup_pid);
	if (p.target_started) {
		int cleanup = protocol_finish(&p);
		(void)cleanup;
	} else {
		{
			irqstate_t flags = enter_critical_section();
			atomic_store(&protocol_active, NULL);
			leave_critical_section(flags);
		}
		ove_sem_deinit(p.block_sem);
		(void)nxsem_destroy(&p.send_gate);
		(void)nxsem_destroy(&p.wait_gate);
		(void)nxsem_destroy(&p.exit_gate);
		(void)nxsem_destroy(&p.setup_gate);
	}
	assert_true(exit_held && setup_waited && setup_completed && gone);
	assert_int_equal(a.result, OVE_ERR_NO_MEMORY);
	assert_true(a.output == (ove_thread_t)(uintptr_t)1);
	assert_int_equal(atomic_load(&p.ready), 0);

	/* Resume never waits for a mutex held by the parked target. */
	protocol_init(&p, storage);
	started = protocol_start(&p, stack, size);
	atomic_store(&p.hold_registry, 1);
	int registry_held = protocol_wait(&p.hold_registry, 2);
	ove_thread_suspend(p.target);
	parked = ove_thread_get_state(p.target);
	ove_thread_resume(p.target);
	joined = protocol_finish(&p);
	assert_true(started && registry_held);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(joined, OVE_OK);

	/* Retire a masked request, then coalesce two callers into a fresh cycle. */
	protocol_init(&p, storage);
	atomic_store(&p.masked, 1);
	started = protocol_start(&p, stack, size);
	a = (struct protocol_caller){.probe = &p};
	b = (struct protocol_caller){.probe = &p};
	protocol_spawn(&p, &ta, &a);
	int pending = protocol_pending(&p);
	ove_thread_resume(p.target);
	int retired = protocol_wait(&a.done, 1);
	protocol_join(&p, ta);
	a = (struct protocol_caller){.probe = &p};
	protocol_spawn(&p, &ta, &a);
	pending &= protocol_pending(&p);
	protocol_spawn(&p, &tb, &b);
	int both_admitted = protocol_phase(&p, 2, 0);
	int sends = atomic_load(&p.send_count);
	atomic_store(&p.unmask, 1);
	int acknowledged = protocol_wait(&a.done, 1) && protocol_wait(&b.done, 1);
	parked = ove_thread_get_state(p.target);
	protocol_join(&p, ta);
	protocol_join(&p, tb);
	joined = protocol_finish(&p);
	assert_true(started && pending && retired && acknowledged && both_admitted);
	assert_int_equal(sends, 2);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(joined, OVE_OK);

	/* Retain a resume between acknowledgement and the first native wait. */
	protocol_init(&p, storage);
	atomic_store(&p.hold_wait, 1);
	started = protocol_start(&p, stack, size);
	ove_thread_suspend(p.target);
	int held = protocol_wait(&p.wait_held, 1);
	for (unsigned int i = 0; i < 100; ++i)
		ove_thread_resume(p.target);
	flags = enter_critical_section();
	p.target->suspend_generation = UINT64_MAX;
	leave_critical_section(flags);
	ove_thread_suspend(p.target);
	flags = enter_critical_section();
	int handler_no_alias = !p.target->suspend_active &&
			       p.target->suspend_generation == UINT64_MAX;
	leave_critical_section(flags);
	int tokens = 0;
	(void)nxsem_get_value(&p.target->suspend_sem, &tokens);
	(void)nxsem_post(&p.wait_gate);
	usleep(30000);
	int restored = ove_thread_get_state(p.target);
	joined = protocol_finish(&p);
	assert_true(started && held && handler_no_alias);
	assert_int_equal(tokens, 1);
	assert_int_equal(restored, OVE_THREAD_STATE_RUNNING);
	assert_int_equal(joined, OVE_OK);

	/* A parked handler must acknowledge a fresh cycle after consuming an
	 * old retained release, preserving the original logical state. */
	protocol_init(&p, storage);
	atomic_store(&p.hold_wait, 1);
	started = protocol_start(&p, stack, size);
	ove_thread_suspend(p.target);
	held = protocol_wait(&p.wait_held, 1);
	ove_thread_resume(p.target);
	a = (struct protocol_caller){.probe = &p};
	protocol_spawn(&p, &ta, &a);
	pending = protocol_pending(&p);
	(void)nxsem_post(&p.wait_gate);
	acknowledged = protocol_wait(&a.done, 1);
	parked = ove_thread_get_state(p.target);
	int stable_count = atomic_load(&p.progress);
	usleep(20000);
	int stayed_parked = atomic_load(&p.progress) == stable_count;
	protocol_join(&p, ta);
	ove_thread_resume(p.target);
	usleep(20000);
	restored = ove_thread_get_state(p.target);
	joined = protocol_finish(&p);
	assert_true(started && held && pending && acknowledged && stayed_parked);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(restored, OVE_THREAD_STATE_RUNNING);
	assert_int_equal(joined, OVE_OK);

	/* An old failed sender cannot retire a newer request or alias at wrap. */
	protocol_init(&p, storage);
	atomic_store(&p.masked, 1);
	atomic_store(&p.hold_send, 1);
	atomic_store(&p.send_error, 1);
	started = protocol_start(&p, stack, size);
	flags = enter_critical_section();
	p.target->suspend_generation = UINT64_MAX - 1;
	leave_critical_section(flags);
	a = (struct protocol_caller){.probe = &p};
	protocol_spawn(&p, &ta, &a);
	held = protocol_wait(&p.send_held, 1);
	ove_thread_resume(p.target);
	/* A live old sender prevents generation reuse. */
	ove_thread_suspend(p.target);
	flags = enter_critical_section();
	int no_alias = !p.target->suspend_active && p.target->suspend_generation == UINT64_MAX;
	/* Use a distinct fresh identity to exercise stale-error isolation. */
	p.target->suspend_generation = 10;
	leave_critical_section(flags);
	b = (struct protocol_caller){.probe = &p};
	protocol_spawn(&p, &tb, &b);
	pending = protocol_pending(&p);
	(void)nxsem_post(&p.send_gate);
	retired = protocol_wait(&a.done, 1);
	flags = enter_critical_section();
	int still_pending = p.target->suspend_active && !p.target->suspend_acked;
	leave_critical_section(flags);
	atomic_store(&p.unmask, 1);
	acknowledged = protocol_wait(&b.done, 1);
	parked = ove_thread_get_state(p.target);
	protocol_join(&p, ta);
	protocol_join(&p, tb);
	joined = protocol_finish(&p);
	assert_true(started && held && pending && retired && no_alias && still_pending &&
		    acknowledged);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(joined, OVE_OK);

	/* Dispatch failure is bounded; finished/close retain the native target
	 * until an admitted sender has returned. */
	protocol_init(&p, storage);
	started = protocol_start(&p, stack, size);
	atomic_store(&p.send_error, 2);
	ove_thread_suspend(p.target);
	int running = ove_thread_get_state(p.target);
	atomic_store(&p.hold_send, 1);
	atomic_store(&p.send_error, 1);
	a = (struct protocol_caller){.probe = &p};
	b = (struct protocol_caller){.probe = &p, .close = 1};
	protocol_spawn(&p, &ta, &a);
	held = protocol_wait(&p.send_held, 1);
	pid_t pid = p.target->pid;
	atomic_store(&p.stop, 1);
	int returned = protocol_wait(&p.entry_returned, 1);
	int sends_before_finish = atomic_load(&p.send_count);
	ove_thread_suspend(p.target);
	int rejected_finished = atomic_load(&p.send_count) == sends_before_finish;
	protocol_spawn(&p, &tb, &b);
	(void)protocol_wait(&b.started, 1);
	usleep(30000);
	flags = enter_critical_section();
	int native_alive = nxsched_get_tcb(pid) != NULL;
	leave_critical_section(flags);
	int close_waiting = !atomic_load(&b.done);
	(void)nxsem_post(&p.send_gate);
	int completed = protocol_wait(&a.done, 1) && protocol_wait(&b.done, 1);
	protocol_join(&p, ta);
	protocol_join(&p, tb);
	{
		irqstate_t flags = enter_critical_section();
		atomic_store(&protocol_active, NULL);
		leave_critical_section(flags);
	}
	ove_sem_deinit(p.block_sem);
	(void)nxsem_destroy(&p.send_gate);
	(void)nxsem_destroy(&p.wait_gate);
	(void)nxsem_destroy(&p.exit_gate);
	(void)nxsem_destroy(&p.setup_gate);
	assert_true(started && held && returned && rejected_finished && native_alive &&
		    close_waiting && completed);
	assert_int_equal(running, OVE_THREAD_STATE_RUNNING);
	assert_int_equal(b.result, OVE_OK);

	/* An error reported after a real acknowledgement cannot release it. */
	protocol_init(&p, storage);
	atomic_store(&p.hold_send, 1);
	atomic_store(&p.send_error, 1);
	started = protocol_start(&p, stack, size);
	a = (struct protocol_caller){.probe = &p};
	protocol_spawn(&p, &ta, &a);
	held = protocol_wait(&p.send_held, 1);
	(void)nxsig_tgkill(-1, p.target->pid, SIGUSR1);
	usleep(30000);
	int ack_before_error = ove_thread_get_state(p.target);
	(void)nxsem_post(&p.send_gate);
	completed = protocol_wait(&a.done, 1);
	usleep(30000);
	int ack_after_error = ove_thread_get_state(p.target);
	protocol_join(&p, ta);
	joined = protocol_finish(&p);
	assert_true(started && held && completed);
	assert_int_equal(ack_before_error, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(ack_after_error, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(joined, OVE_OK);

	/* A direct self park handles a nested reserved-signal prompt once. */
	protocol_init(&p, storage);
	atomic_store(&p.self_park, 1);
	atomic_store(&p.hold_wait, 1);
	started = protocol_start(&p, stack, size);
	held = protocol_wait(&p.wait_held, 1);
	(void)nxsig_tgkill(-1, p.target->pid, SIGUSR1);
	usleep(20000);
	ove_thread_resume(p.target);
	(void)nxsem_post(&p.wait_gate);
	after = protocol_wait(&p.after_self, 1);
	joined = protocol_finish(&p);
	assert_true(started && held && after);
	assert_int_equal(joined, OVE_OK);

	/* List state agrees with acknowledged suspension, and an interrupted
	 * synchronization wait restores its prior logical state. */
	protocol_init(&p, storage);
	atomic_store(&p.block_owner, 1);
	started = protocol_start(&p, stack, size);
	int blocked = protocol_phase(&p, 0, 1);
	int prior = ove_thread_get_state(p.target);
	ove_thread_suspend(p.target);
	size_t count = 0;
	int listed = ove_thread_list(info, capacity, &count);
	int found = 0;
	for (size_t i = 0; i < count; ++i)
		if (info[i].identity == (uintptr_t)(uint32_t)p.target->pid)
			found = info[i].state == OVE_THREAD_STATE_SUSPENDED;
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	flags = enter_critical_section();
	uint64_t suspended_before = p.target->st.cumul_us[OVE_THREAD_STATE_SUSPENDED];
	leave_critical_section(flags);
#endif
	usleep(30000);
	ove_thread_resume(p.target);
	usleep(20000);
	restored = ove_thread_get_state(p.target);
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	flags = enter_critical_section();
	uint64_t suspended_after = p.target->st.cumul_us[OVE_THREAD_STATE_SUSPENDED];
	int tracker_restored = p.target->st.cur_state == prior;
	leave_critical_section(flags);
#endif
	joined = protocol_finish(&p);
	assert_true(started && found && blocked);
	assert_true(listed == OVE_OK || listed == OVE_ERR_QUEUE_FULL);
	assert_int_equal(restored, prior);
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	assert_int_equal(prior, OVE_THREAD_STATE_BLOCKED);
	assert_true(suspended_after > suspended_before && tracker_restored);
#endif
	assert_int_equal(joined, OVE_OK);

	/* Close releases either a still-masked request or an acknowledged park;
	 * an independent native caller remains alive until its call returns. */
	for (int masked = 0; masked < 2; ++masked) {
		protocol_init(&p, storage);
		atomic_store(&p.masked, masked);
		started = protocol_start(&p, stack, size);
		a = (struct protocol_caller){.probe = &p};
		protocol_spawn(&p, &ta, &a);
		if (masked)
			pending = protocol_pending(&p);
		else
			pending = protocol_wait(&a.done, 1);
		ove_thread_request_stop(p.target);
		joined = ove_thread_deinit(p.target);
		completed = protocol_wait(&a.done, 1);
		protocol_join(&p, ta);
		{
			irqstate_t flags = enter_critical_section();
			atomic_store(&protocol_active, NULL);
			leave_critical_section(flags);
		}
		ove_sem_deinit(p.block_sem);
		(void)nxsem_destroy(&p.send_gate);
		(void)nxsem_destroy(&p.wait_gate);
		(void)nxsem_destroy(&p.exit_gate);
		(void)nxsem_destroy(&p.setup_gate);
		assert_true(started && pending && completed);
		assert_int_equal(joined, OVE_OK);
	}

	/* Successful wrap reset and immediate reuse of all caller backing. */
	protocol_init(&p, storage);
	started = protocol_start(&p, stack, size);
	flags = enter_critical_section();
	p.target->suspend_generation = UINT64_MAX;
	leave_critical_section(flags);
	ove_thread_suspend(p.target);
	parked = ove_thread_get_state(p.target);
	joined = protocol_finish(&p);
	assert_true(started);
	assert_int_equal(parked, OVE_THREAD_STATE_SUSPENDED);
	assert_int_equal(joined, OVE_OK);
}
