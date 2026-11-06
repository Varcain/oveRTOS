/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"
#include "ove/irq.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_STACK_BYTES (64u * 1024u * 1024u)

struct slot {
	bool advance, release;
	unsigned callbacks, enters, leaves, stage;
	int first, after_other, after_leave;
	bool ready, region_ok, guard_ok, frame_ok;
	uintptr_t base, end, tls;
	int attr_status, range_status, guard_status, destroy_status;
};
struct start_record {
	void *(*entry)(void *);
	void *arg;
	unsigned index;
	pthread_t thread;
	bool joinable;
};
struct observation {
	pthread_mutex_t lock;
	pthread_cond_t ready;
	struct slot slots[2];
	struct start_record starts[2];
	void *timer_storage[2];
	atomic_bool refused;
	unsigned stack_calls;
	size_t stack_size;
	int stack_status;
	const pthread_attr_t *prepared_attr;
	size_t applied_size, readback, guard_size, guard_readback;
	int apply_status, size_status, guard_status, guard_read_status;
	bool attr_ready, disjoint, tls_distinct;
	unsigned create_attempts, creates, joins;
};
struct callback_arg {
	struct observation *state;
	unsigned index;
};
static struct observation *active;
static _Thread_local int worker_index = -1;

void __real_posix_irq_enter_isr(void) __attribute__((weak));
void __real_posix_irq_leave_isr(void) __attribute__((weak));
int __real_pthread_attr_setstacksize(pthread_attr_t *, size_t);
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);

static void refuse(struct observation *state)
{
	pthread_mutex_lock(&state->lock);
	state->refused = true;
	pthread_cond_broadcast(&state->ready);
	pthread_mutex_unlock(&state->lock);
}

static void *start_thread(void *arg)
{
	struct start_record *record = arg;
	worker_index = (int)record->index;
	struct slot result = {.after_leave = -1};
	pthread_attr_t attr;
	void *base = NULL;
	size_t size = 0, guard = 0;
	/* Sanitized locals may live on a fake stack; observe the native frame. */
	uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
	result.attr_status = pthread_getattr_np(pthread_self(), &attr);
	result.range_status = result.guard_status = result.destroy_status = -1;
	if (!result.attr_status) {
		result.range_status = pthread_attr_getstack(&attr, &base, &size);
		result.guard_status = pthread_attr_getguardsize(&attr, &guard);
		result.destroy_status = pthread_attr_destroy(&attr);
	}
	result.base = (uintptr_t)base;
	result.tls = (uintptr_t)&worker_index;
	if (!result.range_status && result.base && size <= UINTPTR_MAX - result.base) {
		result.end = result.base + size;
		/* The reported region includes native thread bookkeeping. It is not
		 * a claim that every reported byte is available to a callback. */
		result.region_ok = size >= TEST_STACK_BYTES - 1024u * 1024u &&
				   size <= TEST_STACK_BYTES;
		result.frame_ok = frame >= result.base && frame < result.end;
	}
	result.guard_ok = !result.guard_status && guard == active->guard_size;
	result.ready = true;
	pthread_mutex_lock(&active->lock);
	active->slots[record->index] = result;
	bool valid = !result.attr_status && !result.range_status && !result.guard_status &&
		     !result.destroy_status && result.region_ok && result.guard_ok &&
		     result.frame_ok;
	if (!valid)
		active->refused = true;
	pthread_cond_broadcast(&active->ready);
	pthread_mutex_unlock(&active->lock);
	if (!valid)
		return NULL;
	return record->entry(record->arg);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *),
			  void *arg)
{
	if (!active)
		return EINVAL;
	++active->create_attempts;
	size_t size = 0, guard = 0;
	if (!active->attr_ready || attr != active->prepared_attr ||
	    pthread_attr_getstacksize(attr, &size) || size != TEST_STACK_BYTES ||
	    pthread_attr_getguardsize(attr, &guard) || guard != active->guard_size) {
		refuse(active);
		return EINVAL;
	}
	for (unsigned i = 0; i < 2; ++i) {
		if (arg == active->timer_storage[i]) {
			active->starts[i] =
				(struct start_record){.entry = entry, .arg = arg, .index = i};
			int rc = __real_pthread_create(thread, attr, start_thread,
						       &active->starts[i]);
			if (!rc) {
				active->starts[i].thread = *thread;
				active->starts[i].joinable = true;
				++active->creates;
			} else {
				refuse(active);
			}
			return rc;
		}
	}
	refuse(active);
	return EINVAL;
}

int __wrap_pthread_join(pthread_t thread, void **value)
{
	if (!active)
		return EINVAL;
	for (unsigned i = 0; i < 2; ++i) {
		struct start_record *record = &active->starts[i];
		if (record->joinable && pthread_equal(thread, record->thread)) {
			int rc = __real_pthread_join(thread, value);
			if (!rc) {
				record->joinable = false;
				++active->joins;
			} else {
				refuse(active);
			}
			return rc;
		}
	}
	refuse(active);
	return EINVAL;
}

int __wrap_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
	int rc = __real_pthread_attr_setstacksize(attr, size);
	if (active) {
		++active->stack_calls;
		active->stack_size = size;
		active->stack_status = rc;
		active->prepared_attr = attr;
		active->applied_size = TEST_STACK_BYTES;
		active->apply_status = active->size_status = active->guard_status =
			active->guard_read_status = -1;
		long page = sysconf(_SC_PAGESIZE);
		if (rc || size != 512u * 1024u || page <= 0 || page > 1024 * 1024) {
			refuse(active);
			return rc ? rc : EINVAL;
		}
		active->guard_size = (size_t)page;
		active->apply_status = __real_pthread_attr_setstacksize(attr, TEST_STACK_BYTES);
		if (!active->apply_status)
			active->size_status = pthread_attr_getstacksize(attr, &active->readback);
		if (!active->apply_status && !active->size_status &&
		    active->readback == TEST_STACK_BYTES)
			active->guard_status = pthread_attr_setguardsize(attr, active->guard_size);
		if (!active->guard_status)
			active->guard_read_status =
				pthread_attr_getguardsize(attr, &active->guard_readback);
		active->attr_ready = !active->apply_status && !active->size_status &&
				     !active->guard_status && !active->guard_read_status &&
				     active->readback == TEST_STACK_BYTES &&
				     active->guard_readback == active->guard_size;
		if (!active->attr_ready) {
			refuse(active);
			return EINVAL;
		}
	}
	return rc;
}

void __wrap_posix_irq_enter_isr(void)
{
	if (__real_posix_irq_enter_isr)
		__real_posix_irq_enter_isr();
	if (!active)
		return;
	pthread_mutex_lock(&active->lock);
	if (worker_index >= 0 && worker_index < 2)
		++active->slots[worker_index].enters;
	else {
		active->refused = true;
		pthread_cond_broadcast(&active->ready);
	}
	pthread_mutex_unlock(&active->lock);
}

void __wrap_posix_irq_leave_isr(void)
{
	if (__real_posix_irq_leave_isr)
		__real_posix_irq_leave_isr();
	int outside = !ove_is_in_isr();
	if (!active)
		return;
	pthread_mutex_lock(&active->lock);
	if (worker_index >= 0 && worker_index < 2) {
		struct slot *slot = &active->slots[worker_index];
		++slot->leaves;
		slot->after_leave = !outside;
	} else {
		active->refused = true;
		pthread_cond_broadcast(&active->ready);
	}
	pthread_mutex_unlock(&active->lock);
}

static struct timespec deadline(void)
{
	struct timespec result;
	clock_gettime(CLOCK_REALTIME, &result);
	result.tv_sec += 5;
	return result;
}

static void callback(ove_timer_t timer, void *user)
{
	(void)timer;
	struct callback_arg *arg = user;
	struct observation *state = arg->state;
	worker_index = (int)arg->index;
	pthread_mutex_lock(&state->lock);
	struct slot *slot = &state->slots[arg->index];
	slot->first = ove_is_in_isr();
	++slot->callbacks;
	slot->stage = 1;
	pthread_cond_broadcast(&state->ready);
	if (arg->index == 0) {
		struct timespec limit = deadline();
		while (!slot->advance && !state->refused) {
			if (pthread_cond_timedwait(&state->ready, &state->lock, &limit))
				state->refused = true;
		}
		slot->after_other = ove_is_in_isr();
		slot->stage = 2;
		pthread_cond_broadcast(&state->ready);
		limit = deadline();
		while (!slot->release && !state->refused) {
			if (pthread_cond_timedwait(&state->ready, &state->lock, &limit))
				state->refused = true;
		}
	}
	pthread_cond_broadcast(&state->ready);
	pthread_mutex_unlock(&state->lock);
}

static void wait_stage(struct observation *state, unsigned index, unsigned stage)
{
	struct timespec limit = deadline();
	pthread_mutex_lock(&state->lock);
	while (state->slots[index].stage < stage && !state->refused) {
		if (pthread_cond_timedwait(&state->ready, &state->lock, &limit))
			state->refused = true;
	}
	pthread_mutex_unlock(&state->lock);
}

static void wait_resources(struct observation *state)
{
	struct timespec limit = deadline();
	pthread_mutex_lock(&state->lock);
	while ((!state->slots[0].ready || !state->slots[1].ready) && !state->refused) {
		if (pthread_cond_timedwait(&state->ready, &state->lock, &limit))
			state->refused = true;
	}
	if (!state->refused) {
		state->disjoint = state->slots[0].end <= state->slots[1].base ||
				  state->slots[1].end <= state->slots[0].base;
		state->tls_distinct = state->slots[0].tls && state->slots[1].tls &&
				      state->slots[0].tls != state->slots[1].tls;
		if (!state->disjoint || !state->tls_distinct)
			state->refused = true;
	}
	pthread_mutex_unlock(&state->lock);
}

static void check(unsigned *failures, int condition, const char *message)
{
	if (!condition) {
		++*failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "controls") && strcmp(argv[1], "canary")))
		return 2;
	unsigned failures = 0;
	if (!strcmp(argv[1], "canary"))
		check(&failures, 0, "explicit early failure canary");
	struct observation state = {0};
	atomic_init(&state.refused, false);
	state.slots[0].after_leave = state.slots[1].after_leave = -1;
	if (pthread_mutex_init(&state.lock, NULL))
		return 2;
	if (pthread_cond_init(&state.ready, NULL)) {
		pthread_mutex_destroy(&state.lock);
		return 2;
	}
	active = &state;
	int initial_outside = !ove_is_in_isr();
	ove_timer_storage_t storage[2] = {0};
	state.timer_storage[0] = &storage[0];
	state.timer_storage[1] = &storage[1];
	ove_timer_t timers[2] = {NULL, NULL};
	bool created[2] = {false, false};
	struct callback_arg args[2] = {{&state, 0}, {&state, 1}};
	/* Serial creation keeps shared attribute setup single-threaded. The start
     * recorder sets a private worker tag without comparing retired thread IDs. */
	for (unsigned i = 0; i < 2; ++i) {
		if (ove_timer_init_ns(&timers[i], &storage[i], callback, &args[i], 1000000, 1)) {
			state.refused = true;
			break;
		}
		created[i] = true;
	}
	int outside_a = 0, outside_b = 0;
	if (!state.refused)
		wait_resources(&state);
	if (!state.refused) {
		if (ove_timer_start(timers[0]))
			refuse(&state);
		wait_stage(&state, 0, 1);
		outside_a = !ove_is_in_isr();
		if (ove_timer_start(timers[1]))
			refuse(&state);
		wait_stage(&state, 1, 1);
		/* Join B rather than waiting for a LEAVE observer: an omitted LEAVE
         * must produce named observations without a timeout witness. */
		ove_timer_deinit(timers[1]);
		timers[1] = NULL;
		created[1] = false;
		pthread_mutex_lock(&state.lock);
		state.slots[0].advance = true;
		pthread_cond_broadcast(&state.ready);
		pthread_mutex_unlock(&state.lock);
		outside_b = !ove_is_in_isr();
		wait_stage(&state, 0, 2);
	}
	/* Release every callback before joining, including setup/refusal paths. */
	pthread_mutex_lock(&state.lock);
	state.slots[0].advance = state.slots[0].release = true;
	state.slots[1].advance = state.slots[1].release = true;
	pthread_cond_broadcast(&state.ready);
	pthread_mutex_unlock(&state.lock);
	for (unsigned i = 0; i < 2; ++i) {
		if (created[i]) {
			ove_timer_deinit(timers[i]);
			timers[i] = NULL;
		}
	}
	active = NULL;
	pthread_cond_destroy(&state.ready);
	pthread_mutex_destroy(&state.lock);
	if (state.refused || state.creates != 2 || state.joins != 2)
		return 2;
	check(&failures, initial_outside && outside_a && outside_b && !ove_is_in_isr(),
	      "main remains outside overlapping dispatcher contexts");
	check(&failures,
	      state.stack_calls == 1 && state.stack_size == 512u * 1024u && state.stack_status == 0,
	      "dispatcher stack request succeeds before both serial creates");
	check(&failures, __real_posix_irq_enter_isr && __real_posix_irq_leave_isr,
	      "supported timer context has real entry and exit helpers");
	for (unsigned i = 0; i < 2; ++i) {
		check(&failures, state.slots[i].callbacks == 1,
		      "owned one-shot dispatcher calls once");
		check(&failures, state.slots[i].first == 1,
		      "each dispatcher starts inside its own context");
		check(&failures, state.slots[i].enters == 1,
		      "dispatcher forwards one context entry");
		check(&failures, state.slots[i].leaves == 1,
		      "dispatcher forwards one context exit");
		check(&failures, state.slots[i].after_leave == 0,
		      "real exit clears context on the same dispatcher");
	}
	check(&failures, state.slots[0].after_other == 1,
	      "first dispatcher remains inside after the second dispatcher exits");
	printf("%s: failures=%u callbacks=%u,%u first=%d,%d recheck=%d enters=%u,%u leaves=%u,%u after_leave=%d,%d outside=%d,%d,%d stack_calls=%u stack_bytes=%zu stack_status=%d applied_bytes=%zu apply_status=%d readback=%zu size_status=%d guard_bytes=%zu guard_status=%d guard_read_status=%d create_attempts=%u creates=%u joins=%u ready=%d,%d regions=%d,%d guards=%d,%d frames=%d,%d tls_distinct=%d disjoint=%d attr=%d,%d ranges=%d,%d guard_query=%d,%d attr_destroy=%d,%d\n",
	       argv[1], failures, state.slots[0].callbacks, state.slots[1].callbacks,
	       state.slots[0].first, state.slots[1].first, state.slots[0].after_other,
	       state.slots[0].enters, state.slots[1].enters, state.slots[0].leaves,
	       state.slots[1].leaves, state.slots[0].after_leave, state.slots[1].after_leave,
	       initial_outside, outside_a, outside_b, state.stack_calls, state.stack_size,
	       state.stack_status, state.applied_size, state.apply_status, state.readback,
	       state.size_status, state.guard_size, state.guard_status, state.guard_read_status,
	       state.create_attempts, state.creates, state.joins, state.slots[0].ready,
	       state.slots[1].ready, state.slots[0].region_ok, state.slots[1].region_ok,
	       state.slots[0].guard_ok, state.slots[1].guard_ok, state.slots[0].frame_ok,
	       state.slots[1].frame_ok, state.tls_distinct, state.disjoint,
	       state.slots[0].attr_status, state.slots[1].attr_status, state.slots[0].range_status,
	       state.slots[1].range_status, state.slots[0].guard_status,
	       state.slots[1].guard_status, state.slots[0].destroy_status,
	       state.slots[1].destroy_status);
	return failures ? 1 : 0;
}
