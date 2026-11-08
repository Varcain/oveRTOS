/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ove/ove.h"
#include <FreeRTOS.h>
#include <task.h>
#include <event_groups.h>
#include <lwip/dns.h>

/* All RTOS entries below are inert observers of initialized byte objects.
 * The task token is a model type, never a native TCB. No kernel, scheduler,
 * lwIP implementation, network or native blocking operation is linked. */
struct model_task {
	unsigned token;
};
static struct model_task task_token;
enum kind { MUTEX, COUNTER, QUEUE, GROUP };
struct resource {
	void *object;
	enum kind kind;
	bool live;
	unsigned count, maximum, head, tail;
	size_t item_size;
	unsigned char *buffer;
};
static struct resource resources[64];
static unsigned checks, failures, depth, live, allocated, freed;
static unsigned converted, native_calls, posted;
static uint64_t converted_ns;
static TickType_t converted_ticks, arguments[32], now;
static uint64_t wraps;
static BaseType_t native_result;
static bool cv_mode;
static void (*wake)(void);
static ove_stream_t active_stream;
static ove_event_t active_event;
static bool sending;
static unsigned hint_count, release_at;
static int dns_mode;
static bool dns_active;
static dns_found_callback dns_callback;
static void *dns_context;
static SemaphoreHandle_t dns_mutex, dns_sem;
static ip_addr_t dns_value;

static void check(bool valid, const char *name)
{
	++checks;
	if (!valid) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static struct resource *find(void *object)
{
	for (unsigned i = 0; i < 64; ++i)
		if (resources[i].live && resources[i].object == object)
			return &resources[i];
	check(false, "entry uses initialized model storage");
	abort();
}
static void create(void *object, enum kind kind, unsigned count, unsigned maximum)
{
	for (unsigned i = 0; i < 64; ++i)
		if (!resources[i].live) {
			resources[i] = (struct resource){.object = object,
							 .kind = kind,
							 .live = true,
							 .count = count,
							 .maximum = maximum};
			++live;
			return;
		}
	check(false, "bounded model registry has room");
	abort();
}
static void retire(void *object)
{
	find(object)->live = false;
	--live;
}
static void record(TickType_t ticks)
{
	if (native_calls < 32)
		arguments[native_calls] = ticks;
	++native_calls;
}
static uint64_t expected(uint64_t ns)
{
	if (ns == UINT64_MAX)
		return portMAX_DELAY;
	__uint128_t ticks = ((__uint128_t)ns * configTICK_RATE_HZ + 999999999u) / 1000000000u;
	uint64_t cap = (uint64_t)portMAX_DELAY - 1;
	return ticks > cap ? cap : (uint64_t)ticks;
}
void ove_test_conversion(uint64_t ns, TickType_t ticks)
{
	++converted;
	converted_ns = ns;
	converted_ticks = ticks;
}
static void reset(void)
{
	converted = native_calls = posted = hint_count = 0;
	converted_ns = 0;
	converted_ticks = 0;
	memset(arguments, 0, sizeof(arguments));
	now = 100;
	wraps = 0;
	native_result = pdFALSE;
	cv_mode = false;
	wake = NULL;
}
static void conversion(uint64_t ns, bool called)
{
	check(converted == (called ? 1u : 0u),
	      "actual consumer calls conversion exactly when required");
	if (called)
		check(converted_ns == ns && converted_ticks == expected(ns),
		      "consumer conversion equals exact independent budget");
}
void model_enter_critical(void)
{
	++depth;
}
void model_exit_critical(void)
{
	check(depth > 0, "critical sections balance");
	--depth;
}
UBaseType_t model_enter_critical_from_isr(void)
{
	model_enter_critical();
	return depth - 1;
}
void model_exit_critical_from_isr(UBaseType_t value)
{
	check(depth == value + 1, "ISR model critical sections balance");
	model_exit_critical();
}
BaseType_t xPortIsInsideInterrupt(void)
{
	return pdFALSE;
}
void *pvPortMalloc(size_t size)
{
	++allocated;
#ifdef CONFIG_OVE_ZERO_HEAP
	(void)size;
	return NULL;
#else
	return malloc(size);
#endif
}
void vPortFree(void *p)
{
	++freed;
#ifndef CONFIG_OVE_ZERO_HEAP
	free(p);
#else
	(void)p;
#endif
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{
	s->initialized = true;
	create(s, MUTEX, 1, 1);
	if (dns_active && dns_mutex == NULL)
		dns_mutex = s;
	return s;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s)
{
	return xSemaphoreCreateMutexStatic(s);
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(unsigned maximum, unsigned initial,
						 StaticSemaphore_t *s)
{
	s->initialized = true;
	create(s, COUNTER, initial, maximum);
	return s;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s)
{
	SemaphoreHandle_t result = xSemaphoreCreateCountingStatic(1, 0, s);
	dns_sem = result;
	return result;
}
void vSemaphoreDelete(SemaphoreHandle_t s)
{
	retire(s);
	s->initialized = false;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
	struct resource *r = find(s);
	record(ticks);
	if (dns_callback && s == dns_sem && ticks != 0 && dns_mode == 2)
		dns_callback("model", &dns_value, dns_context);
	BaseType_t result = (cv_mode && r->kind == MUTEX) || s == dns_mutex ? pdTRUE
									    : native_result;
	if (s == dns_sem)
		result = (ticks != 0 && dns_mode == 2) ? pdTRUE : pdFALSE;
	if (wake)
		wake();
	if (result == pdTRUE && r->count > 0)
		--r->count;
	return result;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
	struct resource *r = find(s);
	if (r->count < r->maximum)
		++r->count;
	++posted;
	return pdTRUE;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks)
{
	return xSemaphoreTake(s, ticks);
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
	return xSemaphoreGive(s);
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *higher)
{
	*higher = pdFALSE;
	return xSemaphoreGive(s);
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return &task_token;
}
TickType_t xTaskGetTickCount(void)
{
	return now;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	(void)clear;
	record(ticks);
	if (ticks != 0 && active_event)
		active_event->signaled = 1;
	return ticks != 0 && active_event ? 1u : (uint32_t)native_result;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
	check(task == &task_token, "notification uses inert typed task token");
	return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher)
{
	*higher = pdFALSE;
	(void)xTaskNotifyGive(task);
}
void vTaskSuspendAll(void)
{
}
BaseType_t xTaskResumeAll(void)
{
	return pdFALSE;
}
void vTaskSetTimeOutState(TimeOut_t *state)
{
	state->start = now;
	state->wraps = wraps;
}
BaseType_t xTaskCheckForTimeOut(TimeOut_t *state, TickType_t *remaining)
{
	if (*remaining == portMAX_DELAY)
		return pdFALSE;
	TickType_t elapsed = (TickType_t)(now - state->start);
	if ((wraps != state->wraps && now >= state->start) || elapsed >= *remaining) {
		*remaining = 0;
		return pdTRUE;
	}
	*remaining -= elapsed;
	vTaskSetTimeOutState(state);
	return pdFALSE;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t maximum, UBaseType_t size, uint8_t *buffer,
				 StaticQueue_t *storage)
{
	create(storage, QUEUE, 0, maximum);
	struct resource *r = find(storage);
	r->buffer = buffer;
	r->item_size = size;
	return storage;
}
void vQueueDelete(QueueHandle_t q)
{
	retire(q);
}
BaseType_t xQueueSend(QueueHandle_t q, const void *data, TickType_t ticks)
{
	struct resource *r = find(q);
	record(ticks);
	if (native_result != pdPASS)
		return pdFALSE;
	if (r->count >= r->maximum)
		return pdFALSE;
	memcpy(r->buffer + r->head * r->item_size, data, r->item_size);
	r->head = (r->head + 1) % r->maximum;
	++r->count;
	return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *data, TickType_t ticks)
{
	struct resource *r = find(q);
	record(ticks);
	if (native_result != pdPASS || !r->count)
		return pdFALSE;
	memcpy(data, r->buffer + r->tail * r->item_size, r->item_size);
	r->tail = (r->tail + 1) % r->maximum;
	--r->count;
	return pdPASS;
}
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *data, BaseType_t *higher)
{
	*higher = pdFALSE;
	return xQueueSend(q, data, 0);
}
BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *data, BaseType_t *higher)
{
	*higher = pdFALSE;
	return xQueueReceive(q, data, 0);
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
	return find(q)->count;
}
UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t q)
{
	return uxQueueMessagesWaiting(q);
}
UBaseType_t uxQueueGetQueueLength(QueueHandle_t q)
{
	return find(q)->maximum;
}
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *s)
{
	create(s, GROUP, 0, 0);
	return s;
}
void vEventGroupDelete(EventGroupHandle_t g)
{
	retire(g);
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits, BaseType_t clear,
				BaseType_t all, TickType_t ticks)
{
	(void)bits;
	(void)clear;
	(void)all;
	(void)find(g);
	record(ticks);
	return native_result ? 1 : 0;
}
static void stream_wake(void)
{
	++hint_count;
	TickType_t previous = now;
	++now;
	if (now < previous)
		++wraps;
	if (hint_count >= release_at) {
		if (sending)
			ove_ring_clear(&active_stream->ring);
		else {
			const unsigned char data[] = {4, 5};
			(void)ove_ring_write(&active_stream->ring, data, 2);
		}
	}
}
err_t dns_gethostbyname(const char *name, ip_addr_t *value, dns_found_callback callback,
			void *context)
{
	(void)name;
	dns_callback = callback;
	dns_context = context;
	if (dns_mode == 1) {
		*value = dns_value;
		return ERR_OK;
	}
	if (dns_mode == 3)
		return ERR_ARG;
	return ERR_INPROGRESS;
}

static const uint64_t inputs[] = {0,	      1,	  1000000000,	  1000000001,
				  3276750000, 3276800000, UINT64_MAX - 1, UINT64_MAX};
static void queue_cases(void)
{
	ove_queue_storage_t storage;
	unsigned buffer[2], item = 123, out = 0;
	ove_queue_t q;
	check(ove_queue_init(&q, &storage, buffer, sizeof(item), 2) == OVE_OK,
	      "queue initializes model storage");
	for (unsigned i = 0; i < 8; ++i) {
		uint64_t ns = inputs[i];
		reset();
		check(ove_queue_send(q, &item, ns) == (ns ? OVE_ERR_TIMEOUT : OVE_ERR_QUEUE_FULL),
		      "queue send result preserves original timeout classification");
		conversion(ns, ns != UINT64_MAX);
		check(native_calls == 1 && arguments[0] == expected(ns),
		      "queue send submits exact bounded argument");
		reset();
		check(ove_queue_receive(q, &out, ns) ==
			      (ns ? OVE_ERR_TIMEOUT : OVE_ERR_QUEUE_EMPTY),
		      "queue receive preserves empty/timeout classification");
		conversion(ns, ns != UINT64_MAX);
		check(native_calls == 1 && arguments[0] == expected(ns),
		      "queue receive submits exact bounded argument");
	}
	reset();
	native_result = pdPASS;
	check(ove_queue_send(q, &item, UINT64_MAX - 1) == OVE_OK && ove_queue_count(q) == 1,
	      "successful queue send preserves payload/count");
	check(ove_queue_receive(q, &out, UINT64_MAX - 1) == OVE_OK && out == item &&
		      !ove_queue_count(q),
	      "successful queue receive preserves payload/count");
	ove_queue_deinit(q);
#ifndef CONFIG_OVE_ZERO_HEAP
	int rc = ove_queue_create(&q, sizeof(item), 2);
	check(rc == OVE_OK, "common heap queue constructor");
	if (rc == OVE_OK)
		ove_queue_destroy(q);
#endif
}
static void sync_cases(void)
{
	ove_mutex_storage_t ms, rs;
	ove_sem_storage_t ss;
	ove_condvar_storage_t cs;
	ove_event_storage_t es;
	ove_mutex_t m, r;
	ove_sem_t s;
	ove_condvar_t cv;
	ove_event_t event;
	check(ove_mutex_init(&m, &ms) == OVE_OK, "mutex setup");
	check(ove_recursive_mutex_init(&r, &rs) == OVE_OK, "recursive setup");
	check(ove_sem_init(&s, &ss, 0, 2) == OVE_OK, "semaphore setup");
	check(ove_condvar_init(&cv, &cs) == OVE_OK, "condition setup");
	check(ove_event_init(&event, &es) == OVE_OK, "event setup");
	for (unsigned i = 0; i < 8; ++i) {
		uint64_t ns = inputs[i];
		reset();
		check(ove_mutex_lock(m, ns) == OVE_ERR_TIMEOUT, "mutex result preserved");
		conversion(ns, true);
		check(native_calls == 1 && arguments[0] == expected(ns),
		      "mutex bounded native argument");
		reset();
		check(ove_recursive_mutex_lock(r, ns) == OVE_ERR_TIMEOUT,
		      "recursive result preserved");
		conversion(ns, true);
		check(native_calls == 1 && arguments[0] == expected(ns),
		      "recursive bounded native argument");
		reset();
		check(ove_sem_take(s, ns) == OVE_ERR_TIMEOUT, "semaphore result preserved");
		conversion(ns, true);
		check(native_calls == 1 && arguments[0] == expected(ns),
		      "semaphore bounded native argument");
		for (unsigned checked = 0; checked < 2; ++checked) {
			reset();
			native_result = pdTRUE;
			check(ove_mutex_lock(m, 0) == OVE_OK && find(m->sem)->count == 0,
			      "condition caller holds its initialized model mutex");
			reset();
			cv_mode = true;
			bool held = false;
			int result = checked ? ove_condvar_wait_checked(cv, m, ns, &held)
					     : ove_condvar_wait(cv, m, ns);
			check(result == OVE_ERR_TIMEOUT && (!checked || held) && cv->head == NULL,
			      "condition clean timeout reacquires and retires with truthful ownership");
			conversion(ns, true);
			check(native_calls == 2 && arguments[0] == expected(ns) &&
				      arguments[1] == portMAX_DELAY,
			      "condition wait budget and reacquisition remain distinct");
			ove_mutex_unlock(m);
		}
		reset();
		event->signaled = 1;
		active_event = NULL;
		check(ove_event_wait(event, ns) == OVE_OK && event->waiter == NULL &&
			      !event->signaled,
		      "event immediate latch behavior remains unchanged");
		conversion(ns, ns != UINT64_MAX);
		check(native_calls == 1 && arguments[0] == 0,
		      "latched event drains stale hint without claiming full-domain wait accounting");
	}
	reset();
	event->signaled = 0;
	active_event = event;
	check(ove_event_wait(event, UINT64_MAX) == OVE_OK && native_calls == 2 &&
		      arguments[1] == portMAX_DELAY && event->waiter == NULL,
	      "encoded forever event takes inert notification route");
	active_event = NULL;
	ove_event_deinit(event);
	ove_condvar_deinit(cv);
	ove_sem_deinit(s);
	ove_recursive_mutex_deinit(r);
	ove_mutex_deinit(m);
#ifndef CONFIG_OVE_ZERO_HEAP
	int rc = ove_sem_create(&s, 0, 2);
	check(rc == OVE_OK, "common heap semaphore constructor");
	if (rc == OVE_OK)
		ove_sem_destroy(s);
#endif
}
static void group_cases(void)
{
	ove_eventgroup_storage_t storage;
	ove_eventgroup_t g;
	ove_eventbits_t result;
	check(ove_eventgroup_init(&g, &storage) == OVE_OK, "group setup");
	for (unsigned i = 0; i < 8; ++i) {
		uint64_t ns = inputs[i];
		reset();
		result = 99;
		check(ove_eventgroup_wait_bits(g, 1, 0, ns, &result) == OVE_ERR_TIMEOUT &&
			      result == 0,
		      "group timeout/result semantics retained");
		conversion(ns, ns != UINT64_MAX);
		check(native_calls == 1 && arguments[0] == expected(ns),
		      "group submits exact bounded argument");
	}
	ove_eventgroup_deinit(g);
#ifndef CONFIG_OVE_ZERO_HEAP
	int rc = ove_eventgroup_create(&g);
	check(rc == OVE_OK, "common heap group constructor");
	if (rc == OVE_OK)
		ove_eventgroup_destroy(g);
#endif
}
static void stream_cases(void)
{
	ove_stream_storage_t storage;
	unsigned char buffer[2], input[] = {1, 2, 3, 4}, out[2];
	ove_stream_t s;
	size_t count;
	check(ove_stream_init(&s, &storage, buffer, 2, 2) == OVE_OK, "stream setup");
	active_stream = s;
	for (unsigned send = 0; send < 2; ++send) {
		sending = send;
		for (unsigned i = 0; i < 8; ++i) {
			uint64_t ns = inputs[i];
			reset();
			ove_ring_clear(&s->ring);
			if (send)
				ove_ring_write(&s->ring, input, 2);
			wake = stream_wake;
			release_at = 1;
			native_result = pdTRUE;
			int rc = send ? ove_stream_send(s, input, 1, ns, &count)
				      : ove_stream_receive(s, out, 2, ns, &count);
			check(rc == OVE_OK && count == (ns ? (send ? 1u : 2u) : 0u),
			      "stream byte count and zero poll semantics retained");
			conversion(ns, true);
			check(native_calls == (ns ? 1u : 0u) &&
				      (!ns || arguments[0] == expected(ns)),
			      "stream uses exact finite/forever native budget");
		}
		for (unsigned wrap = 0; wrap < 2; ++wrap) {
			reset();
			now = wrap ? (TickType_t)(portMAX_DELAY - 1) : 100;
			ove_ring_clear(&s->ring);
			if (!send)
				ove_ring_write(&s->ring, input, 1);
			wake = stream_wake;
			release_at = 8;
			native_result = pdTRUE;
			uint64_t ns = (UINT64_C(4) * 1000000000 + configTICK_RATE_HZ - 1) /
				      configTICK_RATE_HZ;
			int rc = send ? ove_stream_send(s, input, 4, ns, &count)
				      : ove_stream_receive(s, out, 2, ns, &count);
			check(rc == OVE_OK && count == (send ? 2u : 0u),
			      "stream hint expiration preserves short count and threshold");
			uint64_t budget = expected(ns);
			check(native_calls == budget && arguments[0] == budget &&
				      arguments[native_calls - 1] == 1,
			      "stream keeps one elapsed budget across hints and model wrap");
		}
		reset();
		count = 99;
		int rc = send ? ove_stream_send(s, NULL, 0, UINT64_MAX - 1, &count)
			      : ove_stream_receive(s, NULL, 0, UINT64_MAX - 1, &count);
		check(rc == OVE_OK && count == 0 && !converted && !native_calls,
		      "zero-length stream avoids conversion/native effects");
	}
	ove_stream_deinit(s);
	/* Static FreeRTOS streams have no native delete operation here. Remove
	 * only inert test-registry entries before their backing leaves scope. */
	retire(s->data_sem);
	retire(s->space_sem);
	active_stream = NULL;
#ifndef CONFIG_OVE_ZERO_HEAP
	int rc = ove_stream_create(&s, 2, 2);
	check(rc == OVE_OK, "common heap stream constructor");
	if (rc == OVE_OK) {
		retire(s->data_sem);
		retire(s->space_sem);
		ove_stream_destroy(s);
	}
#endif
}
static void dns_cases(void)
{
	dns_active = true;
	ip_addr_set_zero(&dns_value);
	dns_value.addr = UINT32_C(0x0100007f);
	ove_sockaddr_t address;
	for (unsigned i = 0; i < 8; ++i) {
		reset();
		dns_mode = 0;
		dns_callback = NULL;
		unsigned before = live;
		/* The first initialized mutex is discovered by the ordinary static factory. */
		int rc = ove_dns_resolve("model", &address, inputs[i]);
		if (before != live) {
			for (unsigned j = 0; j < 64; ++j)
				if (resources[j].live && resources[j].kind == MUTEX)
					dns_mutex = resources[j].object;
		}
		check(rc == OVE_ERR_TIMEOUT, "DNS pending timeout result retained");
		conversion(inputs[i] ? inputs[i] : OVE_SEC(10), true);
		check(native_calls == 3 && arguments[0] == portMAX_DELAY && arguments[1] == 0 &&
			      arguments[2] == expected(inputs[i] ? inputs[i] : OVE_SEC(10)),
		      "DNS serialization/default/drain/completion arguments retained");
		unsigned posts_before = posted;
		dns_callback("late", &dns_value, dns_context);
		check(posted == posts_before, "timed-out DNS generation rejects late callback");
	}
	reset();
	dns_mode = 1;
	check(ove_dns_resolve("model", &address, UINT64_MAX - 1) == OVE_OK &&
		      address.family == OVE_AF_INET && !memcmp(address.addr, &dns_value.addr, 4),
	      "cached DNS result retained");
	check(!converted && native_calls == 2, "cached DNS avoids completion wait");
	reset();
	dns_mode = 3;
	check(ove_dns_resolve("model", &address, UINT64_MAX - 1) == OVE_ERR_NET_DNS_FAIL &&
		      !converted && native_calls == 2,
	      "DNS immediate error branch retained");
	reset();
	dns_mode = 2;
	check(ove_dns_resolve("model", &address, UINT64_MAX - 1) == OVE_OK &&
		      !memcmp(address.addr, &dns_value.addr, 4),
	      "pending DNS completion copies callback result");
	conversion(UINT64_MAX - 1, true);
	check(find(dns_mutex)->count == 1, "DNS releases serialization mutex on every return");
	dns_callback = NULL;
	dns_context = NULL;
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		check(false, "early failure survives later controls");
	queue_cases();
	sync_cases();
	group_cases();
	stream_cases();
	dns_cases();
	check(depth == 0 && live == 2, "only inert DNS static state remains after public cleanup");
	check(allocated == freed, "every actual common heap allocation is freed");
#ifdef CONFIG_OVE_ZERO_HEAP
	check(allocated == 0, "zero-heap provider path allocates nothing");
#endif
	/* DNS has no public teardown; retire only test metadata after all callbacks. */
	for (unsigned i = 0; i < 64; ++i)
		resources[i].live = false;
	live = 0;
	check(!live && !dns_callback, "final test-owned inert metadata retirement");
	printf("freertos timeout native observers: bits=%u rate=%" PRIu64
	       " heap=%u checks=%u failures=%u\n",
	       OVE_TICK_BITS, (uint64_t)configTICK_RATE_HZ,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       checks, failures);
	return failures != 0;
}
