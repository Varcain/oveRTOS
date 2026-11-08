/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "checks.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ove/sync.h>

/* The allocation macro discards its size argument in zero-heap mode. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ove_backend_common.h"
#pragma GCC diagnostic pop

/* The include supplies the actual private waiter type, without alias casts. */
#include OVE_SYNC_BACKEND_SOURCE

#ifndef OVE_TEST_NEXT_BEFORE_NOTIFY
#define OVE_TEST_NEXT_BEFORE_NOTIFY 0
#endif

enum operation { NO_OPERATION, SIGNAL, BROADCAST };
enum schedule { ORDINARY, COMMITTED_TIMEOUT, PENDING_NOTIFICATION, IMMEDIATE_TIMEOUT };
struct model_task {
	unsigned int id;
};
struct worker {
	struct model_task token;
	pthread_t thread;
	enum schedule schedule;
	bool started, alive, in_wait, ready, checkpoint, returned, snapshots_cleared;
	bool zero_committed, deliver_zero;
	unsigned int pending, take_calls;
	unsigned int zero_epoch, notice_epoch, checkpoint_epoch;
	struct condvar_waiter *node;
	bool task_field_use, next_field_use, target_use;
	SemaphoreHandle_t deferred_take;
	int result;
};
static pthread_mutex_t scheduler_gate;
static pthread_mutex_t metadata;
static pthread_cond_t changed;
static _Thread_local struct worker *current;
static _Thread_local unsigned int gate_depth, adapter_depth;
static _Thread_local enum operation notifier;
static struct {
	ove_mutex_storage_t mutex_storage;
	ove_condvar_storage_t cv_storage;
	ove_mutex_t mutex;
	ove_condvar_t cv;
	struct worker controller, workers[2];
	unsigned int count, order[2], ordered, notifications;
	int previous, late;
	bool hold_broadcast_frames, post_signal_control;
	unsigned int offers, retentions, repairs, deferred_repairs, post_signal_returns;
	unsigned int epoch, unprotected_notifications;
} world;

static void checkpoint_before_return(SemaphoreHandle_t mutex);
static BaseType_t take_unprotected(SemaphoreHandle_t mutex, TickType_t timeout);

static void require_pthread(int rc)
{
	if (rc)
		fixture_reject("pthread observer operation failed");
}

static struct timespec deadline(void)
{
	struct timespec result;
	if (clock_gettime(CLOCK_REALTIME, &result))
		fixture_reject("observer clock failed");
	result.tv_sec += 5;
	return result;
}

static void lock_metadata(void)
{
	require_pthread(pthread_mutex_lock(&metadata));
}

static void unlock_metadata(void)
{
	require_pthread(pthread_mutex_unlock(&metadata));
}

static void announce(void)
{
	require_pthread(pthread_cond_broadcast(&changed));
}

static void await_true(const bool *value)
{
	const struct timespec until = deadline();
	while (!*value)
		require_pthread(pthread_cond_timedwait(&changed, &metadata, &until));
}

static void enter_gate(void)
{
	require_pthread(pthread_mutex_lock(&scheduler_gate));
	++gate_depth;
}

static void leave_gate(void)
{
	if (!gate_depth)
		fixture_reject("unbalanced observer scheduling gate");
	--gate_depth;
	require_pthread(pthread_mutex_unlock(&scheduler_gate));
}

static struct worker *task_record(TaskHandle_t task)
{
	if (task == &world.controller.token)
		return &world.controller;
	for (unsigned int i = 0; i < world.count; ++i)
		if (task == &world.workers[i].token)
			return &world.workers[i];
	fixture_reject("unknown initialized task token");
}

static bool remaining_use(const struct worker *worker)
{
	return worker->task_field_use || worker->next_field_use || worker->target_use;
}

void model_enter_critical(void)
{
	enter_gate();
	++adapter_depth;
}

void model_exit_critical(void)
{
	if (!adapter_depth)
		fixture_reject("unbalanced adapter critical section");
	--adapter_depth;
	bool wait_for_late = false;
	if (!adapter_depth && notifier != NO_OPERATION) {
		lock_metadata();
		if (notifier == SIGNAL && world.ordered) {
			struct worker *target = &world.workers[world.order[0]];
			target->task_field_use = false;
			target->next_field_use = false;
		}
		if (notifier == BROADCAST && world.notifications == world.ordered) {
			/* This exit is after the reviewed loop's final next-field read. */
			for (unsigned int i = 0; i < world.ordered; ++i)
				world.workers[world.order[i]].next_field_use = false;
			world.hold_broadcast_frames = false;
		}
		if (world.late >= 0) {
			struct worker *late = &world.workers[world.late];
			if (late->zero_committed && !late->deliver_zero) {
				late->deliver_zero = true;
				wait_for_late = world.notifications == 0;
			}
		}
		announce();
		unlock_metadata();
	}
	leave_gate();
	if (wait_for_late) {
		lock_metadata();
		await_true(&world.workers[world.late].checkpoint);
		unlock_metadata();
	}
	if (!adapter_depth && current->deferred_take) {
		SemaphoreHandle_t mutex = current->deferred_take;
		current->deferred_take = NULL;
		lock_metadata();
		++world.deferred_repairs;
		unlock_metadata();
		checkpoint_before_return(mutex);
		if (take_unprotected(mutex, portMAX_DELAY) != pdTRUE)
			fixture_reject("deferred observer acquisition failed");
	}
}

UBaseType_t model_enter_critical_from_isr(void)
{
	UBaseType_t saved = adapter_depth;
	model_enter_critical();
	return saved;
}

void model_exit_critical_from_isr(UBaseType_t saved)
{
	if (adapter_depth != saved + 1)
		fixture_reject("unbalanced saved observer critical section");
	model_exit_critical();
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	if (!current)
		fixture_reject("observer task identity missing");
	return &current->token;
}

TickType_t xTaskGetTickCount(void)
{
	return 0;
}

void vTaskSetTimeOutState(TimeOut_t *state)
{
	(void)state;
	fixture_reject("unexpected Event snapshot in condition-variable observer");
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
	*storage = (StaticSemaphore_t){.initialized = true};
	return storage;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
	lock_metadata();
	if (!mutex->initialized || mutex->owner)
		fixture_reject("model destruction requires an unheld mutex");
	mutex->initialized = false;
	unlock_metadata();
}

static void checkpoint_before_return(SemaphoreHandle_t mutex)
{
	if (!current->in_wait || current == &world.controller)
		return;
	enter_gate();
	lock_metadata();
	if (!current->node)
		fixture_reject("live return checkpoint has no registered node");
	struct condvar_waiter **link = &world.cv->head;
	while (*link && *link != current->node)
		link = &(*link)->next;
	const bool linked = *link != NULL;
	fixture_check(!linked, "waiter unlinked before return");
	if (linked) {
		*link = current->node->next;
		++world.repairs;
	}
	const bool available = mutex->initialized && mutex->owner == NULL;
	const bool offered = available && remaining_use(current);
	if (offered) {
		fixture_check(false, "waiter return offered before notifier last use");
		++world.offers;
	}
	const bool retain = available && (remaining_use(current) || world.hold_broadcast_frames);
	if (retain)
		++world.retentions;
	current->checkpoint = true;
	current->checkpoint_epoch = ++world.epoch;
	announce();
	unlock_metadata();
	leave_gate();
	if (retain) {
		lock_metadata();
		const struct timespec until = deadline();
		while (remaining_use(current) || world.hold_broadcast_frames)
			require_pthread(pthread_cond_timedwait(&changed, &metadata, &until));
		unlock_metadata();
	}
}

static BaseType_t take_unprotected(SemaphoreHandle_t mutex, TickType_t timeout)
{
	if (adapter_depth)
		fixture_reject("observer attempted blocking while protected");
	const struct timespec until = deadline();
	for (;;) {
		enter_gate();
		lock_metadata();
		if (!mutex->initialized)
			fixture_reject("model acquisition requires initialized backing");
		const bool lifetime_ready = !current->in_wait || (!remaining_use(current) &&
								  !world.hold_broadcast_frames);
		if (!mutex->owner && lifetime_ready) {
			mutex->owner = &current->token;
			mutex->depth = 1;
			if (current->in_wait) {
				current->node = NULL;
				current->snapshots_cleared = true;
			}
			unlock_metadata();
			leave_gate();
			return pdTRUE;
		}
		unlock_metadata();
		leave_gate();
		if (timeout == 0)
			return pdFALSE;
		lock_metadata();
		const bool blocked = mutex->owner != NULL ||
				     (current->in_wait &&
				      (remaining_use(current) || world.hold_broadcast_frames));
		if (blocked)
			require_pthread(pthread_cond_timedwait(&changed, &metadata, &until));
		unlock_metadata();
	}
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
	if (current->in_wait && adapter_depth) {
		fixture_check(false, "caller mutex reacquisition stays outside critical");
		current->deferred_take = mutex;
		return pdFALSE;
	}
	checkpoint_before_return(mutex);
	return take_unprotected(mutex, timeout);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
	enter_gate();
	lock_metadata();
	if (!mutex->initialized || mutex->owner != &current->token || mutex->depth != 1)
		fixture_reject("model release requires current ownership");
	if (current->in_wait && !current->node) {
		struct condvar_waiter *node = world.cv->head;
		while (node && node->task != &current->token)
			node = node->next;
		if (!node)
			fixture_reject("registration node missing while live");
		current->node = node;
	}
	mutex->owner = NULL;
	mutex->depth = 0;
	announce();
	unlock_metadata();
	leave_gate();
	return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
	if (!current->in_wait || current == &world.controller ||
	    (clear != pdTRUE && clear != pdFALSE))
		fixture_reject("unexpected model notification take");
	enter_gate();
	lock_metadata();
	const bool first = current->take_calls++ == 0;
	if (first) {
		current->ready = true;
		if (current->schedule == COMMITTED_TIMEOUT) {
			if (current->pending)
				fixture_reject("zero result requires an empty native read");
			current->zero_committed = true;
			current->zero_epoch = ++world.epoch;
			announce();
			unlock_metadata();
			leave_gate();
			lock_metadata();
			await_true(&current->deliver_zero);
			unlock_metadata();
			/* A committed result may be delivered after later posting. */
			enter_gate();
			leave_gate();
			return 0;
		}
		announce();
	}
	unlock_metadata();
	leave_gate();
	const struct timespec until = deadline();
	for (;;) {
		enter_gate();
		lock_metadata();
		const uint32_t result = current->pending;
		if (result || timeout == 0 || (first && current->schedule == IMMEDIATE_TIMEOUT)) {
			if (result)
				current->pending = clear ? 0 : result - 1;
			if (!result && !current->zero_committed) {
				current->zero_committed = true;
				current->zero_epoch = ++world.epoch;
			}
			unlock_metadata();
			leave_gate();
			return result;
		}
		unlock_metadata();
		leave_gate();
		lock_metadata();
		if (!current->pending)
			require_pthread(pthread_cond_timedwait(&changed, &metadata, &until));
		unlock_metadata();
	}
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
	/* Copy the observer identity before the model's final native target use. */
	struct worker *target = task_record(task);
	if (target == &world.controller)
		fixture_reject("notifier target must be a registered waiter");
	const bool protected = adapter_depth != 0;
	const enum operation operation = notifier;
	if (operation == NO_OPERATION)
		fixture_reject("notification has no source-bound notifier operation");
	enter_gate();
	lock_metadata();
	if (!target->alive || target->pending >= 8)
		fixture_reject("notification target is not retained and initialized");
	if (operation == BROADCAST && world.previous >= 0)
		world.workers[world.previous].next_field_use = false;
	target->task_field_use = false;
	if (operation == SIGNAL || OVE_TEST_NEXT_BEFORE_NOTIFY)
		target->next_field_use = false;
	++target->pending;
	target->notice_epoch = ++world.epoch;
	++world.notifications;
	world.previous = (int)(target - world.workers);
	if (!protected)
		++world.unprotected_notifications;
	/* No native target access occurs after this point and inner critical exit. */
	target->target_use = false;
	announce();
	unlock_metadata();
	leave_gate();
	if (!protected) {
		lock_metadata();
		await_true(&target->checkpoint);
		if (operation == SIGNAL && world.post_signal_control) {
			await_true(&target->returned);
			++world.post_signal_returns;
		}
		unlock_metadata();
	}
	return pdPASS;
}

static void notify(enum operation operation)
{
	enter_gate();
	lock_metadata();
	world.ordered = 0;
	world.notifications = 0;
	world.previous = -1;
	struct condvar_waiter *node = world.cv->head;
	while (node) {
		struct worker *worker = task_record(node->task);
		if (worker == &world.controller || world.ordered >= world.count)
			fixture_reject("invalid retained waiter list");
		const unsigned int index = (unsigned int)(worker - world.workers);
		world.order[world.ordered++] = index;
		worker->task_field_use = true;
		worker->next_field_use = true;
		worker->target_use = true;
		if (operation == SIGNAL)
			break;
		node = node->next;
	}
	world.hold_broadcast_frames = operation == BROADCAST && world.ordered != 0;
	unlock_metadata();
	leave_gate();
	notifier = operation;
	if (operation == SIGNAL)
		ove_condvar_signal(world.cv);
	else
		ove_condvar_broadcast(world.cv);
	notifier = NO_OPERATION;
	lock_metadata();
	for (unsigned int i = 0; i < world.ordered; ++i) {
		struct worker *worker = &world.workers[world.order[i]];
		worker->task_field_use = false;
		worker->next_field_use = false;
		worker->target_use = false;
	}
	world.hold_broadcast_frames = false;
	announce();
	unlock_metadata();
}

static void *waiter(void *argument)
{
	current = argument;
	lock_metadata();
	current->alive = true;
	unlock_metadata();
	if (ove_mutex_lock(world.mutex, OVE_WAIT_FOREVER))
		fixture_reject("waiter initial mutex acquisition failed");
	lock_metadata();
	current->in_wait = true;
	unlock_metadata();
	const uint64_t timeout = current->schedule == IMMEDIATE_TIMEOUT ? 0 : 2000000;
	const int rc = ove_condvar_wait(world.cv, world.mutex, timeout);
	lock_metadata();
	if (!current->snapshots_cleared)
		fixture_reject("wait returned without its eligible lifetime checkpoint");
	current->result = rc;
	current->returned = true;
	current->in_wait = false;
	announce();
	unlock_metadata();
	ove_mutex_unlock(world.mutex);
	lock_metadata();
	/* Retained late notifications are fixture cleanup, not an adapter success. */
	current->pending = 0;
	current->alive = false;
	announce();
	unlock_metadata();
	return NULL;
}

static void reset_world(void)
{
	/* All native/model backing from the previous scenario has been closed. */
	world = (typeof(world)){0};
	world.previous = -1;
	world.late = -1;
	world.controller.alive = true;
	world.controller.token.id = 0;
	current = &world.controller;
}

static void exercise(const char *name, unsigned int count, enum operation operation,
		     enum schedule first, bool held, bool post_signal)
{
	reset_world();
	if (count > 2)
		fixture_reject("observer task capacity exceeded");
	world.count = count;
	world.post_signal_control = post_signal;
	if (first == COMMITTED_TIMEOUT)
		world.late = 0;
	if (ove_mutex_init(&world.mutex, &world.mutex_storage) ||
	    ove_condvar_init(&world.cv, &world.cv_storage))
		fixture_reject("observer native setup failed");
	for (unsigned int i = 0; i < count; ++i) {
		struct worker *worker = &world.workers[i];
		worker->token.id = i + 1;
		worker->schedule = i == 0 ? first : ORDINARY;
		worker->pending = worker->schedule == PENDING_NOTIFICATION ? 1 : 0;
		worker->started = true;
		require_pthread(pthread_create(&worker->thread, NULL, waiter, worker));
		lock_metadata();
		await_true(&worker->ready);
		unlock_metadata();
	}
	if (held && ove_mutex_lock(world.mutex, OVE_WAIT_FOREVER))
		fixture_reject("notifier caller-mutex control failed");
	if (operation != NO_OPERATION)
		notify(operation);
	if (held) {
		lock_metadata();
		for (unsigned int i = 0; i < count; ++i)
			await_true(&world.workers[i].checkpoint);
		fixture_check(world.offers == 0, "held caller mutex prevents retirement");
		unlock_metadata();
		ove_mutex_unlock(world.mutex);
	}
	for (unsigned int i = 0; i < count; ++i) {
		const struct timespec until = deadline();
		require_pthread(pthread_timedjoin_np(world.workers[i].thread, NULL, &until));
		const struct worker *worker = &world.workers[i];
		fixture_check(worker->snapshots_cleared, "live snapshots cleared before return");
		fixture_check(worker->result == (worker->schedule == IMMEDIATE_TIMEOUT
							 ? OVE_ERR_TIMEOUT
							 : OVE_OK),
			      "condition wait result preserved");
		if (worker->schedule == COMMITTED_TIMEOUT)
			fixture_check(worker->zero_epoch != 0 &&
					      worker->notice_epoch > worker->zero_epoch,
				      "zero result committed before notification posting");
		if (post_signal)
			fixture_check(worker->checkpoint_epoch > worker->notice_epoch,
				      "post-target signal checkpoint is permitted");
	}
	if (post_signal) {
		fixture_check(world.offers == 0, "completed signal target has no remaining lease");
		if (world.unprotected_notifications)
			fixture_check(world.post_signal_returns == 1,
				      "post-target signal control completes before notify return");
	}
	(void)printf(
		"observed: %s workers=%u notices=%u offers=%u retains=%u unlinks=%u deferred=%u post=%u\n",
		name, count, world.notifications, world.offers, world.retentions, world.repairs,
		world.deferred_repairs, world.post_signal_returns);
	ove_condvar_deinit(world.cv);
	ove_mutex_deinit(world.mutex);
}

static void bootstrap(void)
{
	pthread_mutexattr_t attributes;
	require_pthread(pthread_mutexattr_init(&attributes));
	require_pthread(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE));
	require_pthread(pthread_mutex_init(&scheduler_gate, &attributes));
	require_pthread(pthread_mutexattr_destroy(&attributes));
	require_pthread(pthread_mutex_init(&metadata, NULL));
	require_pthread(pthread_cond_init(&changed, NULL));
	reset_world();
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	bootstrap();
	if (strcmp(argv[1], "signal") == 0) {
		exercise("signal", 1, SIGNAL, ORDINARY, false, false);
	} else if (strcmp(argv[1], "signal_gap") == 0) {
		exercise("signal-timeout-gap", 1, SIGNAL, COMMITTED_TIMEOUT, false, false);
		exercise("post-target-control", 1, SIGNAL, ORDINARY, false, true);
	} else if (strcmp(argv[1], "broadcast") == 0) {
		exercise("broadcast", 2, BROADCAST, ORDINARY, false, false);
		exercise("held-control", 2, BROADCAST, ORDINARY, true, false);
	} else if (strcmp(argv[1], "late_timeout") == 0) {
		exercise("later-detached-timeout", 2, BROADCAST, COMMITTED_TIMEOUT, false, false);
		exercise("held-control", 2, BROADCAST, ORDINARY, true, false);
	} else if (strcmp(argv[1], "spurious") == 0) {
		exercise("pending-success", 1, NO_OPERATION, PENDING_NOTIFICATION, false, false);
		exercise("timeout-control", 1, NO_OPERATION, IMMEDIATE_TIMEOUT, false, false);
	} else if (strcmp(argv[1], "timeout") == 0) {
		exercise("timeout", 1, NO_OPERATION, IMMEDIATE_TIMEOUT, false, false);
	} else if (strcmp(argv[1], "post_signal") == 0) {
		exercise("post-target", 1, SIGNAL, ORDINARY, false, true);
	} else if (strcmp(argv[1], "held_mutex") == 0) {
		exercise("held-signal", 1, SIGNAL, ORDINARY, true, false);
		exercise("held-broadcast", 2, BROADCAST, ORDINARY, true, false);
	} else if (strcmp(argv[1], "empty") == 0) {
		exercise("empty-signal", 0, SIGNAL, ORDINARY, false, false);
		exercise("empty-broadcast", 0, BROADCAST, ORDINARY, false, false);
	} else if (strcmp(argv[1], "canary") == 0) {
		fixture_check(false, "explicit fixture failure canary");
		reset_world();
	} else {
		return 2;
	}
	if (adapter_depth || gate_depth)
		fixture_reject("observer gate survived scenario completion");
	require_pthread(pthread_cond_destroy(&changed));
	require_pthread(pthread_mutex_destroy(&metadata));
	require_pthread(pthread_mutex_destroy(&scheduler_gate));
	return fixture_finish(argv[1]);
}
