/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "framework/ove_test.h"
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/semaphore.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ove_test_nuttx_thread_lifecycle(ove_thread_storage_t *storage, void *stack, size_t size);

int ove_test_nuttx_nxsem_post(sem_t *sem);
int ove_test_nuttx_nxsem_destroy(sem_t *sem);
int ove_test_nuttx_nxsem_wait_uninterruptible(sem_t *sem);
struct tcb_s *ove_test_nuttx_nxsched_get_tcb(pid_t pid);
size_t ove_test_nuttx_nxtask_argvstr(struct tcb_s *tcb, char *args, size_t size);
_Noreturn void ove_test_nuttx_exit(int status);

enum lifetime_mode { LIFETIME_NONE, LIFETIME_POST, LIFETIME_EXIT, LIFETIME_RELAY };
enum replacement_kind { REPLACEMENT_ARGUMENT, REPLACEMENT_PTHREAD, REPLACEMENT_ENTRY };

struct lifetime_probe {
	ove_thread_t target;
	ove_thread_t entry_self;
	_Atomic(ove_thread_t) exit_self;
	pid_t pid;
	atomic_int join_tid;
	sem_t entry_gate;
	sem_t hold_gate;
	atomic_int mode;
	atomic_int held;
	atomic_int relay_taken;
	atomic_int waiters;
	atomic_int observers_done;
	atomic_int join_started;
	atomic_int join_done;
	atomic_int lookups;
	atomic_int replacement;
	atomic_int argv_copies;
	atomic_int deferred_destroy;
	atomic_int group_probe;
	atomic_int group_seen;
	atomic_int group_tid;
	atomic_int group_error;
	atomic_int entry_probe;
	atomic_int entry_returned;
	atomic_int heap_lock_calls;
	int join_result;
	struct tcb_s fake;
};

static _Atomic(struct lifetime_probe *) lifetime_active;

static int lifetime_wait(atomic_int *value, int expected, uint32_t timeout_ms)
{
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	uint64_t deadline = now + (uint64_t)timeout_ms * 1000;
	while (atomic_load(value) != expected) {
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return 0;
		usleep(1000);
	}
	return 1;
}

static int lifetime_reaped(pid_t pid)
{
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	uint64_t deadline = now + 2000000;
	for (;;) {
		irqstate_t flags = enter_critical_section();
		int gone = nxsched_get_tcb(pid) == NULL;
		leave_critical_section(flags);
		if (gone)
			return 1;
		(void)ove_time_get_us(&now);
		if (now >= deadline)
			return 0;
		usleep(1000);
	}
}

int ove_test_nuttx_nxsem_wait_uninterruptible(sem_t *sem)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	if (p && sem == &p->target->done_sem && gettid() != p->pid)
		atomic_fetch_add(&p->waiters, 1);
	leave_critical_section(flags);
	return nxsem_wait_uninterruptible(sem);
}

int ove_test_nuttx_nxsem_post(sem_t *sem)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	int hold = 0;
	if (p && sem == &p->target->done_sem) {
		int mode = atomic_load(&p->mode);
		int owner = gettid() == p->pid;
		hold = (mode == LIFETIME_POST && owner) ||
		       (mode == LIFETIME_RELAY && !owner &&
			atomic_exchange(&p->relay_taken, 1) == 0);
		if (hold)
			atomic_store(&p->held, 1);
	}
	leave_critical_section(flags);
	if (hold) {
		(void)nxsem_wait_uninterruptible(&p->hold_gate);
		atomic_store(&p->held, 0);
	}
	return nxsem_post(sem);
}

int ove_test_nuttx_nxsem_destroy(sem_t *sem)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	int retain = p && sem == &p->target->done_sem;
	if (retain) {
		/* Retain the event until all real target/helper threads have exited. */
		atomic_store(&p->deferred_destroy, 1);
	}
	leave_critical_section(flags);
	return retain ? 0 : nxsem_destroy(sem);
}

_Noreturn void ove_test_nuttx_exit(int status)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	int hold = p && gettid() == p->pid && atomic_load(&p->mode) == LIFETIME_EXIT;
	if (hold)
		atomic_store(&p->exit_self, ove_thread_get_self());
	if (hold)
		atomic_store(&p->held, 1);
	leave_critical_section(flags);
	if (hold) {
		(void)nxsem_wait_uninterruptible(&p->hold_gate);
		atomic_store(&p->held, 0);
	}
	exit(status);
}

struct tcb_s *ove_test_nuttx_nxsched_get_tcb(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	struct tcb_s *fake = NULL;
	if (p && pid == p->pid && gettid() == atomic_load(&p->join_tid)) {
		atomic_fetch_add(&p->lookups, 1);
		if (atomic_load(&p->replacement))
			fake = &p->fake;
	}
	leave_critical_section(flags);
	return fake ? fake : nxsched_get_tcb(pid);
}

size_t ove_test_nuttx_nxtask_argvstr(struct tcb_s *tcb, char *args, size_t size)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	int fake = p && tcb == &p->fake;
	if (fake)
		atomic_fetch_add(&p->argv_copies, 1);
	leave_critical_section(flags);
	return fake ? (size_t)snprintf(args, size, " 0x0") : nxtask_argvstr(tcb, args, size);
}

#ifdef CONFIG_OVE_ZERO_HEAP
void ove_test_nuttx_real_heap_lock(void);
#endif

void ove_heap_lock(void)
{
	irqstate_t flags = enter_critical_section();
	struct lifetime_probe *p = atomic_load(&lifetime_active);
	int intercepted = p != NULL;
	if (intercepted)
		atomic_fetch_add(&p->heap_lock_calls, 1);
	leave_critical_section(flags);
	if (intercepted)
		return;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_test_nuttx_real_heap_lock();
#endif
}

static void *lifetime_observer(void *arg);

static void lifetime_owner(void *arg)
{
	struct lifetime_probe *p = arg;
	p->entry_self = ove_thread_get_self();
	(void)nxsem_wait_uninterruptible(&p->entry_gate);
	if (atomic_load(&p->entry_probe)) {
		ove_thread_start_scheduler();
		ove_run();
		atomic_store(&p->entry_returned, 1);
	}
	if (atomic_load(&p->group_probe)) {
		pthread_attr_t attr;
		int rc = pthread_attr_init(&attr);
		int attr_ready = rc == 0;
		struct sched_param param = {.sched_priority = sched_get_priority_min(SCHED_FIFO)};
		if (!rc)
			rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		if (!rc)
			rc = pthread_attr_setschedparam(&attr, &param);
		pthread_t observer;
		if (!rc)
			rc = pthread_create(&observer, &attr, lifetime_observer, p);
		if (attr_ready)
			(void)pthread_attr_destroy(&attr);
		atomic_store(&p->group_error, rc);
		if (!rc) {
			uint64_t now = 0;
			(void)ove_time_get_us(&now);
			uint64_t deadline = now + 2000000;
			while (!atomic_load(&p->waiters) && !atomic_load(&p->observers_done) &&
			       now < deadline) {
				usleep(1000);
				(void)ove_time_get_us(&now);
			}
			atomic_store(&p->group_seen,
				     atomic_load(&p->waiters) || atomic_load(&p->observers_done));
		}
	}
}

static void *lifetime_joiner(void *arg)
{
	struct lifetime_probe *p = arg;
	atomic_store(&p->join_tid, gettid());
	atomic_store(&p->join_started, 1);
	p->join_result = ove_thread_deinit(p->target);
	atomic_store(&p->join_done, 1);
	return NULL;
}

static void *lifetime_observer(void *arg)
{
	struct lifetime_probe *p = arg;
	if (atomic_load(&p->group_probe))
		atomic_store(&p->group_tid, gettid());
	ove_thread_start_scheduler();
	if (atomic_load(&p->group_probe))
		ove_run();
	atomic_fetch_add(&p->observers_done, 1);
	return NULL;
}

static int lifetime_task_observer(int argc, char *argv[])
{
	(void)argc;
	struct lifetime_probe *p = (void *)(uintptr_t)strtoul(argv[1], NULL, 0);
	(void)lifetime_observer(p);
	return 0;
}

static pid_t lifetime_start_observer(struct lifetime_probe *p)
{
	char argument[2 * sizeof(uintptr_t) + 3];
	(void)snprintf(argument, sizeof(argument), "0x%lx", (unsigned long)(uintptr_t)p);
	char *args[] = {argument, NULL};
	struct sched_param param;
	if (sched_getparam(0, &param) < 0)
		return -1;
	return task_create("scheduler-observer", param.sched_priority, 4096, lifetime_task_observer,
			   args);
}

static void lifetime_join_observer(pid_t pid)
{
	if (!lifetime_reaped(pid)) {
		print_error("scheduler observer did not retire; retaining its context\n");
		for (;;)
			usleep(1000);
	}
}

static void lifetime_start(struct lifetime_probe *p, ove_thread_storage_t *storage, void *stack,
			   size_t size)
{
	memset(p, 0, sizeof(*p));
	assert_int_equal(nxsem_init(&p->entry_gate, 0, 0), 0);
	assert_int_equal(nxsem_set_protocol(&p->entry_gate, SEM_PRIO_NONE), 0);
	assert_int_equal(nxsem_init(&p->hold_gate, 0, 0), 0);
	assert_int_equal(nxsem_set_protocol(&p->hold_gate, SEM_PRIO_NONE), 0);
	OVE_TEST_ASSERT_OK(ove_thread_init(&p->target, storage, "lifetime-owner", lifetime_owner, p,
					   OVE_PRIO_NORMAL, size, stack));
	p->pid = __atomic_load_n(&p->target->pid, __ATOMIC_ACQUIRE);
	irqstate_t flags = enter_critical_section();
	struct tcb_s *native = nxsched_get_tcb(p->pid);
	p->fake.entry = native->entry;
	p->fake.pid = p->pid;
	p->fake.flags = native->flags;
	p->fake.task_state = TSTATE_TASK_RUNNING;
	leave_critical_section(flags);
	flags = enter_critical_section();
	atomic_store(&lifetime_active, p);
	leave_critical_section(flags);
}

static void lifetime_cleanup(struct lifetime_probe *p)
{
	/* Keep the live probe and its gates in scope until the runner's deadline. */
	if (!lifetime_reaped(p->pid)) {
		print_error("native task did not retire; retaining its test context\n");
		for (;;)
			usleep(1000);
	}
	irqstate_t flags = enter_critical_section();
	atomic_store(&lifetime_active, NULL);
	leave_critical_section(flags);
	if (atomic_load(&p->deferred_destroy))
		(void)nxsem_destroy(&p->target->done_sem);
	(void)nxsem_destroy(&p->hold_gate);
	(void)nxsem_destroy(&p->entry_gate);
}

static void lifetime_tail(ove_thread_storage_t *storage, void *stack, size_t size, int mode)
{
	struct lifetime_probe p;
	lifetime_start(&p, storage, stack, size);
	atomic_store(&p.mode, mode);
	pid_t observer = -1;
	int observer_created = 0;
	int observer_ready = 1;
	if (mode == LIFETIME_EXIT) {
		observer = lifetime_start_observer(&p);
		observer_created = observer > 0;
		observer_ready = observer_created && lifetime_wait(&p.waiters, 1, 2000);
	}
	(void)nxsem_post(&p.entry_gate);
	int held = lifetime_wait(&p.held, 1, 2000);
	int observed = mode != LIFETIME_EXIT || lifetime_wait(&p.observers_done, 1, 2000);
	pthread_t joiner;
	int rc = pthread_create(&joiner, NULL, lifetime_joiner, &p);
	int join_started = !rc && lifetime_wait(&p.join_started, 1, 2000);
	usleep(30000);
	int early = atomic_load(&p.join_done);
	(void)nxsem_post(&p.hold_gate);
	if (!rc)
		(void)pthread_join(joiner, NULL);
	else
		p.join_result = ove_thread_deinit(p.target);
	if (observer_created)
		lifetime_join_observer(observer);
	int reaped = lifetime_reaped(p.pid);
	lifetime_cleanup(&p);
	assert_int_equal(rc, 0);
	assert_true(join_started);
	assert_true(observer_ready);
	assert_true(held);
	assert_true(observed);
	assert_false(early);
	assert_int_equal(p.join_result, OVE_OK);
	assert_true(reaped);
	assert_ptr_equal(p.entry_self, storage);
	if (mode == LIFETIME_EXIT)
		assert_null(atomic_load(&p.exit_self));
}

static void lifetime_relay(ove_thread_storage_t *storage, void *stack, size_t size)
{
	struct lifetime_probe p;
	lifetime_start(&p, storage, stack, size);
	atomic_store(&p.mode, LIFETIME_RELAY);
	pid_t observers[2];
	int count = 0;
	while (count < 2) {
		pid_t observer = lifetime_start_observer(&p);
		if (observer < 0)
			break;
		observers[count++] = observer;
	}
	int ready = count == 2 && lifetime_wait(&p.waiters, 2, 2000);
	(void)nxsem_post(&p.entry_gate);
	int held = lifetime_wait(&p.held, 1, 2000);
	if (!held) {
		/* Release missing-relay mutants before reporting their failure. */
		(void)nxsem_post(&p.target->done_sem);
		(void)nxsem_post(&p.target->done_sem);
	}
	pthread_t joiner;
	int rc = pthread_create(&joiner, NULL, lifetime_joiner, &p);
	int join_started = !rc && lifetime_wait(&p.join_started, 1, 2000);
	int reaped = lifetime_reaped(p.pid);
	usleep(30000);
	int early = atomic_load(&p.join_done);
	(void)nxsem_post(&p.hold_gate);
	for (int i = 0; i < count; ++i)
		lifetime_join_observer(observers[i]);
	if (!rc)
		(void)pthread_join(joiner, NULL);
	else
		p.join_result = ove_thread_deinit(p.target);
	int observed = atomic_load(&p.observers_done);
	lifetime_cleanup(&p);
	ove_thread_start_scheduler();
	assert_int_equal(rc, 0);
	assert_true(join_started);
	assert_true(ready);
	assert_true(held);
	assert_true(reaped);
	assert_false(early);
	assert_int_equal(observed, 2);
	assert_int_equal(p.join_result, OVE_OK);
}

static void lifetime_replacement(ove_thread_storage_t *storage, void *stack, size_t size, int state,
				 enum replacement_kind kind)
{
	struct lifetime_probe p;
	lifetime_start(&p, storage, stack, size);
	(void)nxsem_post(&p.entry_gate);
	int reaped = lifetime_reaped(p.pid);
	p.fake.task_state = state;
	if (kind == REPLACEMENT_PTHREAD)
		p.fake.flags = (p.fake.flags & ~TCB_FLAG_TTYPE_MASK) | TCB_FLAG_TTYPE_PTHREAD;
	if (kind == REPLACEMENT_ENTRY)
		p.fake.entry.main = NULL;
	atomic_store(&p.replacement, 1);
	pthread_t joiner;
	int rc = pthread_create(&joiner, NULL, lifetime_joiner, &p);
	int finished = !rc && lifetime_wait(&p.join_done, 1, 100);
	atomic_store(&p.replacement, 0);
	if (!rc)
		(void)pthread_join(joiner, NULL);
	else
		p.join_result = ove_thread_deinit(p.target);
	int copied = atomic_load(&p.argv_copies);
	lifetime_cleanup(&p);
	assert_int_equal(rc, 0);
	assert_true(reaped);
	assert_true(finished);
	assert_int_equal(p.join_result, OVE_OK);
	if (state == TSTATE_TASK_RUNNING && kind == REPLACEMENT_ARGUMENT)
		assert_true(copied > 0);
	else
		assert_int_equal(copied, 0);
}

static void lifetime_group_exit(ove_thread_storage_t *storage, void *stack, size_t size)
{
	struct lifetime_probe p;
	lifetime_start(&p, storage, stack, size);
	atomic_store(&p.group_probe, 1);
	(void)nxsem_post(&p.entry_gate);
	int reaped = lifetime_reaped(p.pid);
	int worker = atomic_load(&p.group_tid);
	int worker_gone = worker > 0 && lifetime_reaped(worker);
	pthread_t joiner;
	int rc = pthread_create(&joiner, NULL, lifetime_joiner, &p);
	int finished = !rc && lifetime_wait(&p.join_done, 1, 100);
	if (!finished && reaped && worker_gone) {
		/* Release a leaked reference only after recording the public failure. */
		irqstate_t flags = enter_critical_section();
		p.target->scheduler_observers = 0;
		leave_critical_section(flags);
	}
	if (!rc)
		(void)pthread_join(joiner, NULL);
	else
		p.join_result = ove_thread_deinit(p.target);
	lifetime_cleanup(&p);
	assert_int_equal(rc, 0);
	assert_int_equal(atomic_load(&p.group_error), 0);
	assert_true(atomic_load(&p.group_seen));
	assert_true(reaped);
	assert_true(worker_gone);
	assert_true(finished);
	assert_int_equal(p.join_result, OVE_OK);
	assert_int_equal(atomic_load(&p.heap_lock_calls), 0);
}

static void lifetime_entry_call(ove_thread_storage_t *storage, void *stack, size_t size)
{
	struct lifetime_probe p;
	lifetime_start(&p, storage, stack, size);
	atomic_store(&p.entry_probe, 1);
	(void)nxsem_post(&p.entry_gate);
	int returned = lifetime_wait(&p.entry_returned, 1, 100);
	if (!returned)
		(void)nxsem_post(&p.target->done_sem);
	OVE_TEST_ASSERT_OK(ove_thread_deinit(p.target));
	lifetime_cleanup(&p);
	assert_true(returned);
	assert_int_equal(atomic_load(&p.heap_lock_calls), 0);
}

void ove_test_nuttx_thread_lifecycle(ove_thread_storage_t *storage, void *stack, size_t size)
{
	lifetime_group_exit(storage, stack, size);
	lifetime_entry_call(storage, stack, size);
	lifetime_tail(storage, stack, size, LIFETIME_POST);
	lifetime_tail(storage, stack, size, LIFETIME_EXIT);
	lifetime_relay(storage, stack, size);
	lifetime_replacement(storage, stack, size, TSTATE_TASK_RUNNING, REPLACEMENT_ARGUMENT);
	lifetime_replacement(storage, stack, size, TSTATE_TASK_INVALID, REPLACEMENT_ARGUMENT);
	lifetime_replacement(storage, stack, size, TSTATE_TASK_INACTIVE, REPLACEMENT_ARGUMENT);
	lifetime_replacement(storage, stack, size, TSTATE_TASK_RUNNING, REPLACEMENT_PTHREAD);
	lifetime_replacement(storage, stack, size, TSTATE_TASK_RUNNING, REPLACEMENT_ENTRY);

	uint64_t first = UINT64_C(0x152638495a6b7c8d);
	uint64_t last = UINT64_C(0x8d7c6b5a49382615);
	unsigned char *bytes = stack;
	for (int cycle = 0; cycle < 8; ++cycle) {
		memcpy(bytes, &first, sizeof(first));
		memcpy(bytes + size - sizeof(last), &last, sizeof(last));
		struct lifetime_probe p;
		lifetime_start(&p, storage, bytes + sizeof(first),
			       size - sizeof(first) - sizeof(last));
		(void)nxsem_post(&p.entry_gate);
		OVE_TEST_ASSERT_OK(ove_thread_deinit(p.target));
		lifetime_cleanup(&p);
		assert_memory_equal(bytes, &first, sizeof(first));
		assert_memory_equal(bytes + size - sizeof(last), &last, sizeof(last));
		memset(bytes + sizeof(first), 0xa5, size - sizeof(first) - sizeof(last));
		memset(storage, 0xa5, sizeof(*storage));
	}
	ove_thread_storage_t other;
	lifetime_tail(&other, stack, size, LIFETIME_EXIT);
}
