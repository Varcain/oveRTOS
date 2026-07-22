/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Two-channel host real-time demonstration for STM32F746G-DISCO:
 *
 *   CH1 / Arduino D3 / PB4 — TIM3_CH1 hardware PWM, 1 kHz, 50 us high
 *   CH2 / Arduino D4 / PG7 — OVE_PRIO_CRITICAL thread response pulse
 *
 * TIM3's update interrupt signals an engine-neutral ove_event. The highest
 * portable host-thread priority waits on that event, raises CH2, performs a
 * fixed calculation, then lowers CH2. CH1 therefore remains a timer-hardware
 * reference even when Linux-personality userspace saturates the CPU, while
 * CH1->CH2 directly exposes interrupt-to-thread dispatch latency.
 */

#include "ove_config.h"

#include "rt_scope.h"
#include "ove/types.h"

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)

#include <stdint.h>

#include "ove/storage.h"
#include "ove/sync.h"
#include "ove/thread.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"
#include "stm32f746xx.h"
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/irq.h>
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <arch/chip/irq.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#else
#error "The STM32 real-time scope demo needs a supported target RTOS engine"
#endif

#define REG32(address) (*(volatile uint32_t *)(uintptr_t)(address))

/* STM32F746 register blocks. Keeping this tiny demonstrator independent of
 * each engine's peripheral-driver model makes the physical experiment exactly
 * the same on FreeRTOS, NuttX and Zephyr. */
#define RT_RCC_BASE 0x40023800u
#define RT_RCC_AHB1ENR REG32(RT_RCC_BASE + 0x30u)
#define RT_RCC_APB1RSTR REG32(RT_RCC_BASE + 0x20u)
#define RT_RCC_APB1ENR REG32(RT_RCC_BASE + 0x40u)
#define RT_RCC_AHB1ENR_GPIOBEN (1u << 1)
#define RT_RCC_AHB1ENR_GPIOGEN (1u << 6)
#define RT_RCC_APB1_TIM3EN (1u << 1)

#define RT_GPIOB_BASE 0x40020400u
#define RT_GPIOG_BASE 0x40021800u
#define RT_GPIO_MODER(base) REG32((base) + 0x00u)
#define RT_GPIO_OTYPER(base) REG32((base) + 0x04u)
#define RT_GPIO_OSPEEDR(base) REG32((base) + 0x08u)
#define RT_GPIO_PUPDR(base) REG32((base) + 0x0cu)
#define RT_GPIO_BSRR(base) REG32((base) + 0x18u)
#define RT_GPIO_AFRL(base) REG32((base) + 0x20u)

#define RT_TIM3_BASE 0x40000400u
#define RT_TIM3_CR1 REG32(RT_TIM3_BASE + 0x00u)
#define RT_TIM3_DIER REG32(RT_TIM3_BASE + 0x0cu)
#define RT_TIM3_SR REG32(RT_TIM3_BASE + 0x10u)
#define RT_TIM3_EGR REG32(RT_TIM3_BASE + 0x14u)
#define RT_TIM3_CCMR1 REG32(RT_TIM3_BASE + 0x18u)
#define RT_TIM3_CCER REG32(RT_TIM3_BASE + 0x20u)
#define RT_TIM3_CNT REG32(RT_TIM3_BASE + 0x24u)
#define RT_TIM3_PSC REG32(RT_TIM3_BASE + 0x28u)
#define RT_TIM3_ARR REG32(RT_TIM3_BASE + 0x2cu)
#define RT_TIM3_CCR1 REG32(RT_TIM3_BASE + 0x34u)

#define RT_TIM_CR1_CEN (1u << 0)
#define RT_TIM_CR1_ARPE (1u << 7)
#define RT_TIM_DIER_UIE (1u << 0)
#define RT_TIM_EGR_UG (1u << 0)
#define RT_TIM_CCMR1_OC1PE (1u << 3)
#define RT_TIM_CCMR1_OC1M_PWM1 (6u << 4)
#define RT_TIM_CCER_CC1E (1u << 0)

#define RT_SCOPE_IRQ 29
#define RT_SCOPE_IRQ_PRIORITY 5
#define RT_SCOPE_STACK_SIZE 1024u
#define RT_SCOPE_WORK_ITERATIONS 512u

/* APB1 timers run at 108 MHz: /108 gives a 1 MHz counter. */
#define RT_SCOPE_PRESCALER 107u
#define RT_SCOPE_PERIOD_TICKS 1000u
#define RT_SCOPE_REFERENCE_HIGH_TICKS 50u

static ove_event_t g_deadline_event;
static ove_event_storage_t g_deadline_event_storage;
static ove_thread_t g_response_thread;
static ove_thread_storage_t g_response_thread_storage;
#if defined(CONFIG_OVE_RTOS_NUTTX)
/* The NuttX backend allocates the requested task stack internally and ignores
 * the caller buffer. Do not spend another scarce 1 KiB of STM32 SRAM on an
 * unused mirror (see ove_thread_init()'s backend contract). */
#define RT_SCOPE_STACK_BYTES RT_SCOPE_STACK_SIZE
#define RT_SCOPE_STACK_PTR NULL
#else
OVE_THREAD_STACK_DEFINE_STATIC_(g_response_stack, RT_SCOPE_STACK_SIZE);
#define RT_SCOPE_STACK_BYTES sizeof(g_response_stack)
#define RT_SCOPE_STACK_PTR g_response_stack
#endif

static volatile uint32_t g_payload_state = 0x6d2b79f5u;
static volatile uint32_t g_deadline_count;
static volatile uint32_t g_response_count;

static inline void response_high(void)
{
	RT_GPIO_BSRR(RT_GPIOG_BASE) = 1u << 7;
}

static inline void response_low(void)
{
	RT_GPIO_BSRR(RT_GPIOG_BASE) = 1u << (7u + 16u);
}

static void timer_update(void)
{
	/* Only the update interrupt is enabled, so clearing the complete status
	 * register is both sufficient and avoids a read/modify/write race. */
	RT_TIM3_SR = 0u;
	__atomic_add_fetch(&g_deadline_count, 1u, __ATOMIC_RELAXED);
	ove_event_signal_from_isr(g_deadline_event);
}

#if defined(CONFIG_OVE_RTOS_FREERTOS)

void TIM3_IRQHandler(void)
{
	timer_update();
}

static int irq_prepare(void)
{
	NVIC_DisableIRQ(TIM3_IRQn);
	NVIC_ClearPendingIRQ(TIM3_IRQn);
	/* Highest interrupt urgency from which FreeRTOS APIs may be called. */
	NVIC_SetPriority(TIM3_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	return OVE_OK;
}

static void rt_scope_irq_enable(void)
{
	NVIC_EnableIRQ(TIM3_IRQn);
}

#elif defined(CONFIG_OVE_RTOS_ZEPHYR)

static void timer_isr(const void *arg)
{
	(void)arg;
	timer_update();
}

static int irq_prepare(void)
{
	irq_disable(RT_SCOPE_IRQ);
	IRQ_CONNECT(RT_SCOPE_IRQ, RT_SCOPE_IRQ_PRIORITY, timer_isr, NULL, 0);
	return OVE_OK;
}

static void rt_scope_irq_enable(void)
{
	irq_enable(RT_SCOPE_IRQ);
}

#elif defined(CONFIG_OVE_RTOS_NUTTX)

static int timer_isr(int irq, void *context, void *arg)
{
	(void)irq;
	(void)context;
	(void)arg;
	timer_update();
	return 0;
}

static int irq_prepare(void)
{
	up_disable_irq(STM32_IRQ_TIM3);
	return irq_attach(STM32_IRQ_TIM3, timer_isr, NULL) < 0 ? OVE_ERR_NOT_REGISTERED : OVE_OK;
}

static void rt_scope_irq_enable(void)
{
	/* NuttX initialises ordinary peripheral IRQs at its highest kernel-callable
	 * priority. A high-priority/zero-latency IRQ could not call nxsem_post(),
	 * which is what ove_event_signal_from_isr() uses on this engine. */
	up_enable_irq(STM32_IRQ_TIM3);
}

#endif

static void response_thread(void *arg)
{
	(void)arg;

	for (;;) {
		if (ove_event_wait(g_deadline_event, OVE_WAIT_FOREVER) != OVE_OK)
			continue;
		uint32_t deadline = __atomic_load_n(&g_deadline_count, __ATOMIC_RELAXED);
		if (deadline == __atomic_load_n(&g_response_count, __ATOMIC_RELAXED))
			continue; /* drain a stale counting-semaphore post on NuttX */

		response_high();

		/* A fixed, observable host workload makes CH2 width useful as well as
		 * its rising edge. The empty asm dependency prevents the compiler from
		 * replacing the loop with a closed-form expression. */
		uint32_t state = g_payload_state;
		for (uint32_t i = 0; i < RT_SCOPE_WORK_ITERATIONS; ++i) {
			state = state * 1664525u + 1013904223u;
			__asm__ volatile("" : "+r"(state));
		}
		g_payload_state = state;
		/* Collapse missed deadlines instead of emitting a misleading burst of
		 * late pulses. A new deadline that arrives during the fixed work leaves
		 * its event pending and is handled on the next iteration. */
		__atomic_store_n(&g_response_count, deadline, __ATOMIC_RELAXED);

		response_low();
	}
}

static void gpio_timer_prepare(void)
{
	/* Enable GPIOB/GPIOG and TIM3, then reset TIM3 to remove state left by a
	 * bootloader or an engine peripheral driver. */
	RT_RCC_AHB1ENR |= RT_RCC_AHB1ENR_GPIOBEN | RT_RCC_AHB1ENR_GPIOGEN;
	RT_RCC_APB1ENR |= RT_RCC_APB1_TIM3EN;
	(void)RT_RCC_APB1ENR;
	RT_RCC_APB1RSTR |= RT_RCC_APB1_TIM3EN;
	RT_RCC_APB1RSTR &= ~RT_RCC_APB1_TIM3EN;

	/* PB4 = AF2 / TIM3_CH1, push-pull, very high speed, no pulls. */
	RT_GPIO_MODER(RT_GPIOB_BASE) = (RT_GPIO_MODER(RT_GPIOB_BASE) & ~(3u << (4u * 2u))) |
				       (2u << (4u * 2u));
	RT_GPIO_OTYPER(RT_GPIOB_BASE) &= ~(1u << 4);
	RT_GPIO_OSPEEDR(RT_GPIOB_BASE) |= 3u << (4u * 2u);
	RT_GPIO_PUPDR(RT_GPIOB_BASE) &= ~(3u << (4u * 2u));
	RT_GPIO_AFRL(RT_GPIOB_BASE) = (RT_GPIO_AFRL(RT_GPIOB_BASE) & ~(0xfu << (4u * 4u))) |
				      (2u << (4u * 4u));

	/* PG7 = response GPIO, push-pull, very high speed, initially low. */
	response_low();
	RT_GPIO_MODER(RT_GPIOG_BASE) = (RT_GPIO_MODER(RT_GPIOG_BASE) & ~(3u << (7u * 2u))) |
				       (1u << (7u * 2u));
	RT_GPIO_OTYPER(RT_GPIOG_BASE) &= ~(1u << 7);
	RT_GPIO_OSPEEDR(RT_GPIOG_BASE) |= 3u << (7u * 2u);
	RT_GPIO_PUPDR(RT_GPIOG_BASE) &= ~(3u << (7u * 2u));

	/* 1 MHz timer counter, 1 kHz period, 50 us hardware reference pulse. */
	RT_TIM3_CR1 = 0u;
	RT_TIM3_DIER = 0u;
	RT_TIM3_PSC = RT_SCOPE_PRESCALER;
	RT_TIM3_ARR = RT_SCOPE_PERIOD_TICKS - 1u;
	RT_TIM3_CCR1 = RT_SCOPE_REFERENCE_HIGH_TICKS;
	RT_TIM3_CCMR1 = RT_TIM_CCMR1_OC1PE | RT_TIM_CCMR1_OC1M_PWM1;
	RT_TIM3_CCER = RT_TIM_CCER_CC1E;
	RT_TIM3_EGR = RT_TIM_EGR_UG;
	RT_TIM3_SR = 0u;

	/* Begin one tick before overflow so the first scheduled CH1 rising edge and
	 * update interrupt correspond. The operator still discards setup transients. */
	RT_TIM3_CNT = RT_SCOPE_PERIOD_TICKS - 1u;
}

int linux_rt_scope_start(void)
{
	int rc = ove_event_init(&g_deadline_event, &g_deadline_event_storage);
	if (rc != OVE_OK)
		return rc;

	rc = ove_thread_init(&g_response_thread, &g_response_thread_storage, "rt-scope",
			     response_thread, NULL, OVE_PRIO_CRITICAL, RT_SCOPE_STACK_BYTES,
			     RT_SCOPE_STACK_PTR);
	if (rc != OVE_OK) {
		ove_event_deinit(g_deadline_event);
		return rc;
	}

	rc = irq_prepare();
	if (rc != OVE_OK)
		return rc;

	gpio_timer_prepare();
	rt_scope_irq_enable();
	RT_TIM3_DIER = RT_TIM_DIER_UIE;
	RT_TIM3_CR1 = RT_TIM_CR1_ARPE | RT_TIM_CR1_CEN;
	return OVE_OK;
}

#else /* !CONFIG_OVE_LINUX_RT_SCOPE */

int linux_rt_scope_start(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_LINUX_RT_SCOPE */
