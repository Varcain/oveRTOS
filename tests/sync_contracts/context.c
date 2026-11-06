/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/irq.h"
#include <stdio.h>
#include <string.h>

static unsigned native_locks, native_unlocks, bad_native, lock_controls;
#ifndef CONFIG_OVE_RTOS_POSIX
static bool interrupt_context;
static unsigned mask;
#endif

#ifdef CONFIG_OVE_RTOS_FREERTOS
#include "FreeRTOS.h"
BaseType_t xPortIsInsideInterrupt(void)
{
	return interrupt_context;
}
#ifdef CONFIG_OVE_ASYNC
static unsigned depth;
void model_enter_critical(void)
{
	++native_locks;
	if (interrupt_context)
		++bad_native;
	else
		++depth;
}
void model_exit_critical(void)
{
	++native_unlocks;
	if (!depth)
		++bad_native;
	else
		--depth;
}
UBaseType_t model_enter_critical_from_isr(void)
{
	++native_locks;
	unsigned previous = mask;
	mask = 1;
	return previous;
}
void model_exit_critical_from_isr(UBaseType_t previous)
{
	++native_unlocks;
	mask = previous;
}
#endif
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
bool k_is_in_isr(void)
{
	return interrupt_context;
}
#ifdef CONFIG_OVE_ASYNC
unsigned irq_lock(void)
{
	++native_locks;
	unsigned previous = mask;
	mask = 1;
	return previous;
}
void irq_unlock(unsigned previous)
{
	++native_unlocks;
	mask = previous;
}
#endif
#elif defined(CONFIG_OVE_RTOS_NUTTX)
bool up_interrupt_context(void)
{
	return interrupt_context;
}
#ifdef CONFIG_OVE_ASYNC
unsigned enter_critical_section(void)
{
	++native_locks;
	unsigned previous = mask;
	mask = 1;
	return previous;
}
void leave_critical_section(unsigned previous)
{
	++native_unlocks;
	mask = previous;
}
#endif
#else
void posix_irq_enter_isr(void) __attribute__((weak));
void posix_irq_leave_isr(void) __attribute__((weak));
#ifdef CONFIG_OVE_ASYNC
#include <pthread.h>
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_lock(mutex);
	native_locks += rc == 0;
	return rc;
}
int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_unlock(mutex);
	native_unlocks += rc == 0;
	return rc;
}
#endif
#endif

static void check(unsigned *failures, int condition, const char *message)
{
	if (!condition) {
		++*failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

static void nested_locks(unsigned *failures, bool inside)
{
	unsigned before_locks = native_locks, before_unlocks = native_unlocks;
#ifndef CONFIG_OVE_RTOS_POSIX
	mask = 37;
#endif
	ove_irq_key_t outer = ove_irq_lock();
	ove_irq_key_t inner = ove_irq_lock();
	ove_irq_unlock(inner);
	check(failures, ove_is_in_isr() == inside, "critical sections preserve interrupt context");
	ove_irq_unlock(outer);
#ifdef CONFIG_OVE_ASYNC
	check(failures, native_locks == before_locks + 2 && native_unlocks == before_unlocks + 2,
	      "async critical sections enter and leave the native provider");
#else
	check(failures,
	      outer == 0 && inner == 0 && native_locks == before_locks &&
		      native_unlocks == before_unlocks,
	      "disabled critical sections remain inert");
#endif
#ifndef CONFIG_OVE_RTOS_POSIX
	check(failures, mask == 37, "nested critical sections restore saved mask state");
#ifdef CONFIG_OVE_RTOS_FREERTOS
#ifdef CONFIG_OVE_ASYNC
	check(failures, depth == 0, "thread critical nesting is balanced");
#endif
#endif
#endif
	check(failures, bad_native == 0, "critical native dispatch uses valid context and nesting");
	++lock_controls;
}

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "controls") && strcmp(argv[1], "canary")))
		return 2;
	unsigned failures = 0;
	if (!strcmp(argv[1], "canary"))
		check(&failures, 0, "explicit early failure canary");
	if (ove_is_in_isr())
		return 2;
	nested_locks(&failures, false);
#if defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC)
#ifdef CONFIG_OVE_RTOS_POSIX
	if (!posix_irq_enter_isr || !posix_irq_leave_isr) {
		check(&failures, 0, "supported context query has live simulator helpers");
	} else {
		posix_irq_enter_isr();
		check(&failures, ove_is_in_isr(),
		      "supported context query observes interrupt context");
		nested_locks(&failures, true);
		posix_irq_leave_isr();
	}
#else
	interrupt_context = true;
	check(&failures, ove_is_in_isr(), "supported context query observes interrupt context");
	nested_locks(&failures, true);
	interrupt_context = false;
#endif
#endif
	if (ove_is_in_isr())
		return 2;
	printf("%s: failures=%u outside=0 lock_controls=%u native_locks=%u native_unlocks=%u bad_native=%u\n",
	       argv[1], failures, lock_controls, native_locks, native_unlocks, bad_native);
	return failures ? 1 : 0;
}
