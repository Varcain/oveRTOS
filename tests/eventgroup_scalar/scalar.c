/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef OVE_SCALAR_HAS_QUERY
#define OVE_SCALAR_HAS_QUERY 1
#endif
static ove_eventgroup_storage_t backing;
static unsigned checks, failures, effects, calls, queues, yields, notifications;
static unsigned enters, leaves, bad;
static bool inject_getter, consume_after_test, consumer_pending;
static uint32_t consumed;
static uint32_t wanted_mask;

static void check(bool value, const char *name)
{
	++checks;
	if (!value) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static void notified(void *p)
{
	(void)p;
	++notifications;
	++effects;
}
int ove_time_get_ns(uint64_t *out)
{
	++effects;
	*out = 5;
	return OVE_OK;
}

#ifdef CONFIG_OVE_RTOS_FREERTOS
#include <event_groups.h>
static StaticEventGroup_t native;
static EventBits_t native_bits;
static BaseType_t queue_result = pdPASS;
void model_enter_critical(void)
{
	++enters;
	++effects;
}
void model_exit_critical(void)
{
	++leaves;
	++effects;
}
UBaseType_t model_enter_critical_from_isr(void)
{
	model_enter_critical();
	return 7;
}
void model_exit_critical_from_isr(UBaseType_t key)
{
	if (key != 7)
		++bad;
	model_exit_critical();
}
void vTaskSuspendAll(void)
{
	++effects;
}
BaseType_t xTaskResumeAll(void)
{
	++effects;
	return pdFALSE;
}
BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t fn, void *p, uint32_t bits,
					 BaseType_t *wake)
{
	(void)fn;
	(void)p;
	(void)bits;
	*wake = pdFALSE;
	++queues;
	++effects;
	return queue_result;
}
static void yielded(BaseType_t value)
{
	(void)value;
	++yields;
	++effects;
}
#undef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR(value) yielded(value)
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *p)
{
	return p;
}
void vEventGroupDelete(EventGroupHandle_t p)
{
	(void)p;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t p, EventBits_t bits)
{
	if (p != &native)
		++bad;
	++calls;
	++effects;
	native_bits |= bits;
	return native_bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t p, EventBits_t bits)
{
	if (p != &native)
		++bad;
	++calls;
	++effects;
	EventBits_t previous = native_bits;
	native_bits &= ~bits;
	return previous;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t p)
{
	if (p != &native)
		++bad;
	++calls;
	++effects;
	return native_bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t p, EventBits_t bits, BaseType_t clear,
				BaseType_t all, TickType_t timeout)
{
	(void)timeout;
	if (p != &native)
		++bad;
	++calls;
	++effects;
	EventBits_t previous = native_bits;
	bool matches = all ? (native_bits & bits) == bits : (native_bits & bits) != 0;
	if (matches && clear)
		native_bits &= ~bits;
	return previous;
}
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t p, EventBits_t bits, BaseType_t *wake)
{
	(void)bits;
	if (p != &native)
		++bad;
	*wake = pdFALSE;
	++queues;
	++effects;
	return queue_result;
}
BaseType_t xEventGroupClearBitsFromISR(EventGroupHandle_t p, EventBits_t bits)
{
	(void)bits;
	if (p != &native)
		++bad;
	++queues;
	++effects;
	return queue_result;
}
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
static bool locked;
static bool consume_before_lock;
static unsigned trace_enters, trace_exits;
static uint32_t trace_cleared, trace_mask;
static uint32_t native_wait(struct k_event *, uint32_t, bool, bool);
void ove_model_event_trace_enter(struct k_event *event, uint32_t value, uint32_t mask)
{
	++trace_enters;
	if (event != &backing.event || !locked || value != 0)
		++bad;
	trace_mask = mask;
}
void ove_model_event_trace_exit(struct k_event *event, uint32_t value, uint32_t mask)
{
	++trace_exits;
	if (event != &backing.event || locked || mask != trace_mask)
		++bad;
	trace_cleared = value;
}

void k_event_init(struct k_event *event)
{
	memset(event, 0, sizeof(*event));
}
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	if (lock != &backing.event.lock || locked)
		++bad;
	if (consume_before_lock) {
		consume_before_lock = false;
		consumed = native_wait(&backing.event, 1, true, false);
	}
	locked = true;
	++effects;
	++enters;
	return 7;
}
void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	if (lock != &backing.event.lock || !locked || key != 7)
		++bad;
	locked = false;
	++effects;
	++leaves;
}
static uint32_t native_wait(struct k_event *event, uint32_t mask, bool clear, bool all)
{
	k_spinlock_key_t key = k_spin_lock(&event->lock);
	uint32_t match = event->events & mask;
	if (all && match != mask)
		match = 0;
	if (clear)
		event->events &= ~match;
	k_spin_unlock(&event->lock, key);
	return match;
}
uint32_t k_event_test(struct k_event *event, uint32_t mask)
{
	++calls;
	uint32_t previous = native_wait(event, mask, false, false);
	if (consume_after_test && consumer_pending) {
		consumer_pending = false;
		consumed = native_wait(event, 1, true, false);
	}
	return previous;
}
uint32_t k_event_post(struct k_event *event, uint32_t bits)
{
	++calls;
	k_spinlock_key_t key = k_spin_lock(&event->lock);
	uint32_t previous = event->events & bits;
	event->events |= bits;
	k_spin_unlock(&event->lock, key);
	return previous;
}
uint32_t k_event_clear(struct k_event *event, uint32_t bits)
{
	++calls;
	k_spinlock_key_t key = k_spin_lock(&event->lock);
	uint32_t previous = event->events & bits;
	event->events &= ~bits;
	k_spin_unlock(&event->lock, key);
	return previous;
}
uint32_t k_event_wait(struct k_event *event, uint32_t bits, bool reset, k_timeout_t timeout)
{
	(void)reset;
	(void)timeout;
	++calls;
	return native_wait(event, bits, false, false);
}
uint32_t k_event_wait_all(struct k_event *event, uint32_t bits, bool reset, k_timeout_t timeout)
{
	(void)reset;
	(void)timeout;
	++calls;
	return native_wait(event, bits, false, true);
}
uint32_t k_event_wait_safe(struct k_event *event, uint32_t bits, bool reset, k_timeout_t timeout)
{
	(void)reset;
	(void)timeout;
	++calls;
	return native_wait(event, bits, true, false);
}
uint32_t k_event_wait_all_safe(struct k_event *event, uint32_t bits, bool reset,
			       k_timeout_t timeout)
{
	(void)reset;
	(void)timeout;
	++calls;
	return native_wait(event, bits, true, true);
}
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
irqstate_t enter_critical_section(void)
{
	++effects;
	++enters;
	if (inject_getter)
		backing.bits = 2;
	return 7;
}
void leave_critical_section(irqstate_t key)
{
	if (key != 7 || enters != leaves + 1)
		++bad;
	++effects;
	++leaves;
}
int nxsem_init(sem_t *p, int shared, int32_t value)
{
	(void)p;
	(void)shared;
	(void)value;
	return 0;
}
int nxsem_destroy(sem_t *p)
{
	(void)p;
	return 0;
}
int nxsem_post(sem_t *p)
{
	(void)p;
	++effects;
	return 0;
}
int nxsem_wait_uninterruptible(sem_t *p)
{
	(void)p;
	++effects;
	return -ETIMEDOUT;
}
int nxsem_tickwait_uninterruptible(sem_t *p, uint32_t ticks)
{
	(void)p;
	(void)ticks;
	++effects;
	return -ETIMEDOUT;
}
clock_t clock_systime_ticks(void)
{
	++effects;
	return 10;
}
bool clock_compare(clock_t a, clock_t b)
{
	return a <= b;
}
#else
static int observed_lock(pthread_mutex_t *p)
{
	(void)p;
	++effects;
	++enters;
	return 0;
}
static int observed_unlock(pthread_mutex_t *p)
{
	(void)p;
	++effects;
	++leaves;
	return 0;
}
static int observed_broadcast(pthread_cond_t *p)
{
	(void)p;
	++effects;
	return 0;
}
static int observed_wait(pthread_cond_t *p, pthread_mutex_t *m, const struct timespec *t)
{
	(void)p;
	(void)m;
	(void)t;
	++effects;
	return ETIMEDOUT;
}
static int observed_clock(clockid_t id, struct timespec *t)
{
	(void)id;
	++effects;
	t->tv_sec = 0;
	t->tv_nsec = 0;
	return 0;
}
#define pthread_mutex_lock observed_lock
#define pthread_mutex_unlock observed_unlock
#define pthread_cond_broadcast observed_broadcast
#define pthread_cond_timedwait observed_wait
#define clock_gettime observed_clock
#endif

#include OVE_SCALAR_PROVIDER

static void reset(uint32_t bits)
{
	memset(&backing, 0, sizeof(backing));
	backing.notify_cb = notified;
#ifdef CONFIG_OVE_RTOS_FREERTOS
	backing.handle = &native;
	native_bits = bits;
	queue_result = pdPASS;
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	backing.event.events = bits;
	locked = false;
	consume_before_lock = false;
	trace_enters = trace_exits = trace_cleared = trace_mask = 0;
#else
	backing.bits = bits;
#endif
	effects = calls = queues = yields = notifications = enters = leaves = bad = 0;
	inject_getter = consume_after_test = consumer_pending = false;
	consumed = 0;
}
static uint32_t state(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	return (uint32_t)native_bits;
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	return backing.event.events;
#else
	return backing.bits;
#endif
}
static void wait_admission(void)
{
	const uint32_t masks[] = {0, 0, 0, 0, 1, 1, 1};
	const uint32_t flags[] = {0, 1, 2, 3, 4, UINT32_C(0x80000000), 7};
	for (unsigned i = 0; i < 7; ++i) {
		reset(0);
		uint32_t result = 0x55;
		int rc = ove_eventgroup_wait_bits(&backing, masks[i], flags[i], 0, &result);
		check(rc == OVE_ERR_INVALID_PARAM && result == 0x55 && effects == 0 && state() == 0,
		      i < 4 ? "zero wait rejected before effects"
			    : "unknown flags rejected before effects");
	}
	reset(0);
	uint32_t result = 0x55;
	int rc = ove_eventgroup_wait_bits_until(&backing, 0, 0, 20, &result);
	check(rc == OVE_ERR_INVALID_PARAM && result == 0x55 && effects == 0,
	      "invalid deadline wait avoids clock and native effects");
	reset(0);
	result = 0x55;
	rc = ove_eventgroup_wait_bits_until(&backing, 1, 4, 20, &result);
	check(rc == OVE_ERR_INVALID_PARAM && result == 0x55 && effects == 0,
	      "unknown deadline flags avoid clock and native effects");
	if (wanted_mask != UINT32_MAX) {
		reset(0);
		result = 0x55;
		rc = ove_eventgroup_wait_bits_until(&backing, wanted_mask + 1, 0, 20, &result);
		check(rc == OVE_ERR_INVALID_PARAM && result == 0x55 && effects == 0,
		      "invalid deadline mask avoids clock and native effects");
	}
}
static void mask_admission(void)
{
	if (wanted_mask == UINT32_MAX)
		return;
	const uint32_t invalid[] = {wanted_mask + 1, UINT32_C(0x80000000), wanted_mask + 2,
				    UINT32_C(0x00010000)};
	for (unsigned i = 0; i < 4; ++i) {
		uint32_t mask = invalid[i];
		if ((mask & ~wanted_mask) == 0)
			mask = wanted_mask + 1;
		for (unsigned op = 0; op < 5; ++op) {
			reset(1);
			uint32_t result = 0x55;
			int status = 0;
			switch (op) {
			case 0:
				result = ove_eventgroup_set_bits(&backing, mask);
				break;
			case 1:
				result = ove_eventgroup_clear_bits(&backing, mask);
				break;
			case 2:
				status = ove_eventgroup_wait_bits(&backing, mask, 0, 0, &result);
				break;
			case 3:
				result = ove_eventgroup_set_bits_from_isr(&backing, mask);
				break;
			default:
				status = ove_eventgroup_clear_bits_from_isr(&backing, mask);
				break;
			}
			bool rejected = op == 2 ? status == OVE_ERR_INVALID_PARAM && result == 0x55
					: op == 4 ? status == OVE_ERR_INVALID_PARAM
						  : result == 0;
			check(rejected && effects == 0 && state() == 1,
			      "public mask rejected before narrowing native or queue effects");
		}
	}
}
static void ordinary(void)
{
	reset(3);
	check(ove_eventgroup_set_bits(&backing, 0) == 3 && state() == 3 && notifications == 0,
	      "zero set preserves snapshot without notify");
	reset(3);
	check(ove_eventgroup_clear_bits(&backing, 0) == 3 && state() == 3 && notifications == 0,
	      "zero clear preserves full prior");
	reset(3);
	check(ove_eventgroup_clear_bits(&backing, 1) == 3 && state() == 2,
	      "clear returns unaffected prior bits");
	reset(3);
	check(ove_eventgroup_clear_bits(&backing, 4) == 3 && state() == 3,
	      "clearing absent bit returns full prior");
	reset(0);
	check(ove_eventgroup_set_bits(&backing, wanted_mask) == wanted_mask &&
		      state() == wanted_mask && notifications == 1,
	      "all configured public bits are usable");
	check(ove_eventgroup_clear_bits(&backing, wanted_mask) == wanted_mask && state() == 0,
	      "ordinary clear all uses configured mask");
	for (uint32_t flags = 0; flags <= 3; ++flags) {
		reset(3);
		uint32_t result = 0;
		int rc = ove_eventgroup_wait_bits(&backing, 1, flags, 0, &result);
		check(rc == OVE_OK && (result & 1) == 1 && state() == ((flags & 2) ? 2u : 3u),
		      "known flags preserve ordinary poll semantics");
	}
}
static void exclusion(void)
{
#ifdef CONFIG_OVE_RTOS_NUTTX
	reset(1);
	inject_getter = true;
	uint32_t bits = ove_eventgroup_get_bits(&backing);
	check(bits == 2 && enters == 1 && leaves == 1 && bad == 0,
	      "getter snapshot uses writer exclusion");
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	reset(3);
	uint32_t traced = ove_eventgroup_clear_bits(&backing, 1);
	check(traced == 3 && state() == 2 && trace_enters == 1 && trace_exits == 1 &&
		      trace_cleared == 2 && bad == 0,
	      "native trace arguments and lock intervals preserved");
	reset(3);
	consume_after_test = consumer_pending = true;
	uint32_t previous = ove_eventgroup_clear_bits(&backing, 1);
	if (consumer_pending) {
		consumer_pending = false;
		consumed = native_wait(&backing.event, 1, true, false);
	}
	check(((consumed == 1 && previous == 2) || (consumed == 0 && previous == 3)) &&
		      state() == 2 && enters == leaves && bad == 0,
	      "clear and successful consumer have one legal ordering");
	reset(3);
	consume_before_lock = true;
	previous = ove_eventgroup_clear_bits(&backing, 1);
	check(consumed == 1 && previous == 2 && state() == 2 && enters == leaves && bad == 0,
	      "consumer before clear yields updated full prior");
#endif
}
int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "canary")))
		return 2;
#ifdef CONFIG_OVE_RTOS_FREERTOS
#if OVE_MODEL_TICK_BITS == 16
	wanted_mask = UINT32_C(0xff);
#elif OVE_MODEL_TICK_BITS == 64
	wanted_mask = UINT32_MAX;
#else
	wanted_mask = UINT32_C(0xffffff);
#endif
#else
	wanted_mask = UINT32_MAX;
#endif
	if (argc == 2)
		check(0, "early failure survives controls");
#if OVE_SCALAR_HAS_QUERY
	reset(0);
	check(ove_eventgroup_valid_mask() == wanted_mask && effects == 0,
	      "ordinary configured mask query has no native effects");
#endif
	wait_admission();
	mask_admission();
	ordinary();
	exclusion();
	printf("eventgroup scalar: checks=%u failures=%u\n", checks, failures);
	return failures != 0;
}
