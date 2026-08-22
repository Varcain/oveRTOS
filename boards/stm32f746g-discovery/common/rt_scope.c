/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * STM32F746G-DISCO physical probe used by the Linux interop demo.
 */

#include "ove/hal/hal_rt_scope.h"

#include <stddef.h>
#include <stdint.h>

#include "ove_config.h"
#include "ove/storage.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"
#include "stm32f746xx.h"
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/irq.h>
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <arch/chip/irq.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <sched.h>
#else
#error "The STM32 real-time scope probe needs a supported target RTOS engine"
#endif

#define REG32(address) (*(volatile uint32_t *)(uintptr_t)(address))

#define RT_RCC_BASE 0x40023800u
#define RT_RCC_AHB1ENR REG32(RT_RCC_BASE + 0x30u)
#define RT_RCC_APB1RSTR REG32(RT_RCC_BASE + 0x20u)
#define RT_RCC_APB1ENR REG32(RT_RCC_BASE + 0x40u)
#define RT_RCC_AHB1ENR_GPIOBEN (1u << 1)
#define RT_RCC_AHB1ENR_GPIOGEN (1u << 6)
#define RT_RCC_APB1_TIM3EN (1u << 1)
#define RT_RCC_APB1_TIM5EN (1u << 3)

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

#define RT_TIM5_BASE 0x40000c00u
#define RT_TIM5_CR1 REG32(RT_TIM5_BASE + 0x00u)
#define RT_TIM5_EGR REG32(RT_TIM5_BASE + 0x14u)
#define RT_TIM5_CNT REG32(RT_TIM5_BASE + 0x24u)
#define RT_TIM5_PSC REG32(RT_TIM5_BASE + 0x28u)
#define RT_TIM5_ARR REG32(RT_TIM5_BASE + 0x2cu)

#define RT_TIM_CR1_CEN (1u << 0)
#define RT_TIM_CR1_ARPE (1u << 7)
#define RT_TIM_DIER_UIE (1u << 0)
#define RT_TIM_EGR_UG (1u << 0)
#define RT_TIM_CCMR1_OC1PE (1u << 3)
#define RT_TIM_CCMR1_OC1M_PWM1 (6u << 4)
#define RT_TIM_CCER_CC1E (1u << 0)

#define RT_SCOPE_IRQ 29
#define RT_SCOPE_IRQ_PRIORITY 0
#define RT_SCOPE_TIM3_PRESCALER 1u
#define RT_SCOPE_REFERENCE_HIGH_TICKS (50u * OVE_RT_SCOPE_TICKS_PER_US)
#define RT_SCOPE_TIM5_PRESCALER 107u
#define RT_SCOPE_STACK_SIZE 1024u
#define RT_SCOPE_REPORT_STACK_SIZE 1024u

#if !defined(CONFIG_OVE_RTOS_NUTTX)
OVE_THREAD_STACK_DEFINE_STATIC_(g_response_stack, RT_SCOPE_STACK_SIZE);
OVE_THREAD_STACK_DEFINE_STATIC_(g_report_stack, RT_SCOPE_REPORT_STACK_SIZE);
#endif

static ove_hal_rt_scope_release_fn g_release;

ove_hal_rt_scope_stack_t ove_hal_rt_scope_worker_stack(ove_hal_rt_scope_worker_t worker)
{
	const size_t size = worker == OVE_HAL_RT_SCOPE_RESPONSE_WORKER ? RT_SCOPE_STACK_SIZE
								       : RT_SCOPE_REPORT_STACK_SIZE;
#if defined(CONFIG_OVE_RTOS_NUTTX)
	/* NuttX's kernel heap is DTCM-backed on this target. Static application
	 * buffers live in the tighter and slower SRAM1 region. */
	return (ove_hal_rt_scope_stack_t){.size = size, .buffer = NULL};
#else
	return (ove_hal_rt_scope_stack_t){
		.size = size,
		.buffer = worker == OVE_HAL_RT_SCOPE_RESPONSE_WORKER ? g_response_stack
								     : g_report_stack,
	};
#endif
}

#if defined(CONFIG_OVE_RTOS_FREERTOS)

void TIM3_IRQHandler(void)
{
	g_release();
}

int ove_hal_rt_scope_irq_prepare(ove_hal_rt_scope_release_fn release)
{
	if (release == NULL)
		return OVE_ERR_INVALID_PARAM;
	g_release = release;
	NVIC_DisableIRQ(TIM3_IRQn);
	NVIC_ClearPendingIRQ(TIM3_IRQn);
	NVIC_SetPriority(TIM3_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	return OVE_OK;
}

static void rt_scope_irq_enable(void)
{
	NVIC_EnableIRQ(TIM3_IRQn);
}

#elif defined(CONFIG_OVE_RTOS_ZEPHYR)

_Static_assert(RT_SCOPE_IRQ_PRIORITY == 0,
	       "the Zephyr scope reference must remain the highest ordinary IRQ");

static void timer_isr(const void *arg)
{
	(void)arg;
	g_release();
}

int ove_hal_rt_scope_irq_prepare(ove_hal_rt_scope_release_fn release)
{
	if (release == NULL)
		return OVE_ERR_INVALID_PARAM;
	g_release = release;
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
	g_release();
	return 0;
}

int ove_hal_rt_scope_irq_prepare(ove_hal_rt_scope_release_fn release)
{
	if (release == NULL)
		return OVE_ERR_INVALID_PARAM;
	g_release = release;
	up_disable_irq(STM32_IRQ_TIM3);
	return irq_attach(STM32_IRQ_TIM3, timer_isr, NULL) < 0 ? OVE_ERR_NOT_REGISTERED : OVE_OK;
}

static void rt_scope_irq_enable(void)
{
	up_enable_irq(STM32_IRQ_TIM3);
}

#endif

void ove_hal_rt_scope_hardware_prepare(void)
{
	RT_RCC_AHB1ENR |= RT_RCC_AHB1ENR_GPIOBEN | RT_RCC_AHB1ENR_GPIOGEN;
	RT_RCC_APB1ENR |= RT_RCC_APB1_TIM3EN | RT_RCC_APB1_TIM5EN;
	(void)RT_RCC_APB1ENR;
	RT_RCC_APB1RSTR |= RT_RCC_APB1_TIM3EN | RT_RCC_APB1_TIM5EN;
	RT_RCC_APB1RSTR &= ~(RT_RCC_APB1_TIM3EN | RT_RCC_APB1_TIM5EN);

	RT_GPIO_MODER(RT_GPIOB_BASE) = (RT_GPIO_MODER(RT_GPIOB_BASE) & ~(3u << (4u * 2u))) |
				       (2u << (4u * 2u));
	RT_GPIO_OTYPER(RT_GPIOB_BASE) &= ~(1u << 4);
	RT_GPIO_OSPEEDR(RT_GPIOB_BASE) |= 3u << (4u * 2u);
	RT_GPIO_PUPDR(RT_GPIOB_BASE) &= ~(3u << (4u * 2u));
	RT_GPIO_AFRL(RT_GPIOB_BASE) = (RT_GPIO_AFRL(RT_GPIOB_BASE) & ~(0xfu << (4u * 4u))) |
				      (2u << (4u * 4u));

	ove_hal_rt_scope_response_set(0);
	RT_GPIO_MODER(RT_GPIOG_BASE) = (RT_GPIO_MODER(RT_GPIOG_BASE) & ~(3u << (7u * 2u))) |
				       (1u << (7u * 2u));
	RT_GPIO_OTYPER(RT_GPIOG_BASE) &= ~(1u << 7);
	RT_GPIO_OSPEEDR(RT_GPIOG_BASE) |= 3u << (7u * 2u);
	RT_GPIO_PUPDR(RT_GPIOG_BASE) &= ~(3u << (7u * 2u));

	RT_TIM3_CR1 = 0u;
	RT_TIM3_DIER = 0u;
	RT_TIM3_PSC = RT_SCOPE_TIM3_PRESCALER;
	RT_TIM3_ARR = OVE_RT_SCOPE_PERIOD_TICKS - 1u;
	RT_TIM3_CCR1 = RT_SCOPE_REFERENCE_HIGH_TICKS;
	RT_TIM3_CCMR1 = RT_TIM_CCMR1_OC1PE | RT_TIM_CCMR1_OC1M_PWM1;
	RT_TIM3_CCER = RT_TIM_CCER_CC1E;
	RT_TIM3_EGR = RT_TIM_EGR_UG;
	RT_TIM3_SR = 0u;

	RT_TIM5_CR1 = 0u;
	RT_TIM5_PSC = RT_SCOPE_TIM5_PRESCALER;
	RT_TIM5_ARR = UINT32_MAX;
	RT_TIM5_CNT = 0u;
	RT_TIM5_EGR = RT_TIM_EGR_UG;
	RT_TIM5_CR1 = RT_TIM_CR1_CEN;

	RT_TIM3_CNT = OVE_RT_SCOPE_PERIOD_TICKS - 1u;
}

void ove_hal_rt_scope_start(void)
{
	rt_scope_irq_enable();
	RT_TIM3_DIER = RT_TIM_DIER_UIE;
	RT_TIM3_CR1 = RT_TIM_CR1_ARPE | RT_TIM_CR1_CEN;
}

void ove_hal_rt_scope_irq_ack(void)
{
	RT_TIM3_SR = 0u;
}

uint32_t ove_hal_rt_scope_phase_ticks(void)
{
	return RT_TIM3_CNT;
}

uint32_t ove_hal_rt_scope_time_us(void)
{
	return RT_TIM5_CNT;
}

void ove_hal_rt_scope_response_set(int high)
{
	RT_GPIO_BSRR(RT_GPIOG_BASE) = 1u << (high ? 7u : 23u);
}

int ove_hal_rt_scope_release_attribution(uint32_t *preempt_locked, uint32_t *owner_pid)
{
	if (!preempt_locked || !owner_pid)
		return OVE_ERR_INVALID_PARAM;
#if defined(CONFIG_OVE_RTOS_NUTTX)
	*preempt_locked = sched_lockcount() > 0 ? 1u : 0u;
	*owner_pid = *preempt_locked ? (uint32_t)nxsched_getpid() : 0u;
	return OVE_OK;
#else
	*preempt_locked = 0u;
	*owner_pid = 0u;
	return OVE_ERR_NOT_SUPPORTED;
#endif
}

int ove_hal_rt_scope_release_attribution_available(void)
{
#if defined(CONFIG_OVE_RTOS_NUTTX)
	return 1;
#else
	return 0;
#endif
}
