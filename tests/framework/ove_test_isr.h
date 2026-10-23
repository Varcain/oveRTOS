/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Run a test function in interrupt context, for the _from_isr APIs:
 * a watchdog callback on NuttX, a k_timer expiry on Zephyr and the SysTick
 * tick hook on FreeRTOS (the FreeRTOS lanes' sim_main.c forward it here).
 * The host has no interrupts: there the function runs on the caller.
 */

#ifndef OVE_TEST_ISR_H
#define OVE_TEST_ISR_H

#include <stdbool.h>
#include <stdint.h>

#include "ove/thread.h"

#if defined(CONFIG_OVE_RTOS_NUTTX)
#include <nuttx/arch.h>
#include <nuttx/wdog.h>
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
/* Set by ove_test_run_in_isr(), called and cleared by the lane's
 * vApplicationTickHook (sim_main.c). */
extern void (*volatile ove_test_tick_fn)(void *);
extern void *volatile ove_test_tick_arg;
#endif

struct ove_test_isr_call_ {
	void (*fn)(void *);
	void *arg;
	volatile int done;
	volatile int in_isr;
};

static inline void ove_test_isr_trampoline_(struct ove_test_isr_call_ *c)
{
#if defined(CONFIG_OVE_RTOS_NUTTX)
	c->in_isr = up_interrupt_context();
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	c->in_isr = k_is_in_isr();
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
	uint32_t ipsr;
	__asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	c->in_isr = ipsr != 0;
#else
	c->in_isr = 0;
#endif
	c->fn(c->arg);
	__atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
}

#if defined(CONFIG_OVE_RTOS_NUTTX)
static inline void ove_test_isr_wd_(wdparm_t arg)
{
	ove_test_isr_trampoline_((struct ove_test_isr_call_ *)arg);
}
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
static inline void ove_test_isr_timer_(struct k_timer *t)
{
	ove_test_isr_trampoline_(k_timer_user_data_get(t));
}
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
static inline void ove_test_isr_tick_(void *arg)
{
	ove_test_isr_trampoline_(arg);
}
#endif

/* Runs fn(arg) once, from an interrupt where the lane has one, and waits
 * for it (up to a second).  Returns whether it ran in interrupt context. */
static inline bool ove_test_run_in_isr(void (*fn)(void *), void *arg)
{
	struct ove_test_isr_call_ c = {.fn = fn, .arg = arg};
#if defined(CONFIG_OVE_RTOS_NUTTX)
	struct wdog_s wd = {0};
	if (wd_start(&wd, 1, ove_test_isr_wd_, (wdparm_t)&c) != 0)
		return false;
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	struct k_timer t;
	k_timer_init(&t, ove_test_isr_timer_, NULL);
	k_timer_user_data_set(&t, &c);
	k_timer_start(&t, K_MSEC(1), K_NO_WAIT);
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
	ove_test_tick_arg = &c;
	ove_test_tick_fn = ove_test_isr_tick_;
#else
	ove_test_isr_trampoline_(&c);
#endif
	for (int i = 0; i < 1000 && !__atomic_load_n(&c.done, __ATOMIC_ACQUIRE); i++)
		ove_thread_sleep_ms(1);
#if defined(CONFIG_OVE_RTOS_NUTTX)
	(void)wd_cancel(&wd);
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	k_timer_stop(&t);
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
	ove_test_tick_fn = NULL;
#endif
	return __atomic_load_n(&c.done, __ATOMIC_ACQUIRE) && c.in_isr;
}

#endif /* OVE_TEST_ISR_H */
