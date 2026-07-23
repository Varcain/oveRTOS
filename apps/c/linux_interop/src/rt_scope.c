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
 * CH1->CH2 directly exposes interrupt-to-thread dispatch latency. TIM5 is a
 * pinless 1 MHz timebase used to recover the number of hardware releases when
 * a long interrupt mask collapses multiple TIM3 updates into one pending IRQ.
 */

#include "ove_config.h"

#include "rt_scope.h"
#include "ove/types.h"

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)

#include <stdint.h>

#include "ove/storage.h"
#include "ove/sync.h"
#include "ove/thread.h"
#include "ove/time.h"

#if defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"
#include "lxp/lxp_syscall.h"
#include "ove_freertos_lnx_metrics.h"
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
#define RT_SCOPE_IRQ_PRIORITY 5
#define RT_SCOPE_STACK_SIZE 1024u
#define RT_SCOPE_REPORT_STACK_SIZE 1024u
#define RT_SCOPE_WORK_ITERATIONS 512u
#define RT_SCOPE_REPORT_PERIOD_MS 10000u

/* APB1 timers run at 108 MHz. TIM3 runs at 54 MHz so its phase counter gives
 * 18.5 ns dispatch resolution while remaining inside its 16-bit ARR. TIM5 is
 * divided to 1 MHz and wraps after about 71 minutes. */
#define RT_SCOPE_TIM3_PRESCALER 1u
#define RT_SCOPE_TICKS_PER_US 54u
#define RT_SCOPE_PERIOD_US 1000u
#define RT_SCOPE_PERIOD_TICKS (RT_SCOPE_PERIOD_US * RT_SCOPE_TICKS_PER_US)
#define RT_SCOPE_REFERENCE_HIGH_TICKS (50u * RT_SCOPE_TICKS_PER_US)
#define RT_SCOPE_TIM5_PRESCALER 107u
#define RT_SCOPE_HIST_BINS 18u

static const uint16_t g_hist_upper_us[RT_SCOPE_HIST_BINS] = {
	1u, 2u, 3u, 4u, 5u, 6u, 8u, 10u, 12u, 16u, 20u, 32u, 50u, 100u, 250u, 500u, 750u, 1000u,
};

static ove_event_t g_deadline_event;
static ove_event_storage_t g_deadline_event_storage;
static ove_thread_t g_response_thread;
static ove_thread_storage_t g_response_thread_storage;
static ove_thread_t g_report_thread;
static ove_thread_storage_t g_report_thread_storage;
#if defined(CONFIG_OVE_RTOS_NUTTX)
/* The NuttX backend allocates requested task stacks internally and ignores
 * caller buffers. Do not spend scarce STM32 SRAM on unused mirrors. */
#define RT_SCOPE_STACK_BYTES RT_SCOPE_STACK_SIZE
#define RT_SCOPE_STACK_PTR NULL
#define RT_SCOPE_REPORT_STACK_BYTES RT_SCOPE_REPORT_STACK_SIZE
#define RT_SCOPE_REPORT_STACK_PTR NULL
#else
OVE_THREAD_STACK_DEFINE_STATIC_(g_response_stack, RT_SCOPE_STACK_SIZE);
OVE_THREAD_STACK_DEFINE_STATIC_(g_report_stack, RT_SCOPE_REPORT_STACK_SIZE);
#define RT_SCOPE_STACK_BYTES sizeof(g_response_stack)
#define RT_SCOPE_STACK_PTR g_response_stack
#define RT_SCOPE_REPORT_STACK_BYTES sizeof(g_report_stack)
#define RT_SCOPE_REPORT_STACK_PTR g_report_stack
#endif

static volatile uint32_t g_payload_state = 0x6d2b79f5u;
static volatile uint32_t g_deadline_count;
static volatile uint32_t g_response_count;
static volatile uint32_t g_irq_overrun_count;
static uint32_t g_last_deadline_us;
static volatile uint32_t g_last_execution_deadline;
static volatile uint32_t g_total_executions;
static volatile uint32_t g_total_missed;
static volatile uint32_t g_total_late_finish;
static volatile uint32_t g_total_generation;
static linux_rt_scope_write_fn g_report_write;

struct rt_scope_metrics {
	uint32_t executions;
	uint32_t missed;
	uint32_t late_finish;
	uint32_t dispatch_min_ticks;
	uint32_t dispatch_max_ticks;
	uint64_t dispatch_sum_ticks;
	uint32_t work_min_ticks;
	uint32_t work_max_ticks;
	uint32_t dispatch_hist[RT_SCOPE_HIST_BINS];
};

static volatile uint32_t g_active_metrics;
static struct rt_scope_metrics g_metrics[2] = {
	{
		.dispatch_min_ticks = UINT32_MAX,
		.work_min_ticks = UINT32_MAX,
	},
	{
		.dispatch_min_ticks = UINT32_MAX,
		.work_min_ticks = UINT32_MAX,
	},
};

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
	/* TIM3 phase identifies the latest physical CH1 edge. Combining it with
	 * the 32-bit TIM5 timebase recovers how many 1 ms releases elapsed even
	 * when the TIM3 update flag was already pending. */
	uint32_t now_us = RT_TIM5_CNT;
	uint32_t phase_us = RT_TIM3_CNT / RT_SCOPE_TICKS_PER_US;
	uint32_t deadline_us = now_us - phase_us;
	uint32_t elapsed_us = deadline_us - g_last_deadline_us;
	uint32_t periods = (elapsed_us + RT_SCOPE_PERIOD_US / 2u) / RT_SCOPE_PERIOD_US;

	if (periods == 0u)
		periods = 1u;
	g_last_deadline_us += periods * RT_SCOPE_PERIOD_US;
	RT_TIM3_SR = 0u;
	if (periods > 1u)
		__atomic_add_fetch(&g_irq_overrun_count, periods - 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_deadline_count, periods, __ATOMIC_RELEASE);
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

static uint32_t histogram_bin(uint32_t ticks)
{
	for (uint32_t i = 0; i < RT_SCOPE_HIST_BINS; ++i) {
		if (ticks <= (uint32_t)g_hist_upper_us[i] * RT_SCOPE_TICKS_PER_US)
			return i;
	}
	return RT_SCOPE_HIST_BINS - 1u;
}

static void metrics_record(uint32_t deadline, uint32_t dispatch_ticks, uint32_t work_ticks,
			   uint32_t missed, int late_finish)
{
	uint32_t active = __atomic_load_n(&g_active_metrics, __ATOMIC_ACQUIRE);
	struct rt_scope_metrics *metrics = &g_metrics[active];

	metrics->executions++;
	metrics->missed += missed;
	if (late_finish)
		metrics->late_finish++;
	if (dispatch_ticks < metrics->dispatch_min_ticks)
		metrics->dispatch_min_ticks = dispatch_ticks;
	if (dispatch_ticks > metrics->dispatch_max_ticks)
		metrics->dispatch_max_ticks = dispatch_ticks;
	metrics->dispatch_sum_ticks += dispatch_ticks;
	if (work_ticks < metrics->work_min_ticks)
		metrics->work_min_ticks = work_ticks;
	if (work_ticks > metrics->work_max_ticks)
		metrics->work_max_ticks = work_ticks;
	metrics->dispatch_hist[histogram_bin(dispatch_ticks)]++;
	__atomic_add_fetch(&g_total_generation, 1u, __ATOMIC_ACQ_REL);
	__atomic_add_fetch(&g_total_executions, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_total_missed, missed, __ATOMIC_RELAXED);
	if (late_finish)
		__atomic_add_fetch(&g_total_late_finish, 1u, __ATOMIC_RELAXED);
	__atomic_store_n(&g_last_execution_deadline, deadline, __ATOMIC_RELEASE);
	__atomic_add_fetch(&g_total_generation, 1u, __ATOMIC_RELEASE);
}

static void response_thread(void *arg)
{
	(void)arg;

	for (;;) {
		if (ove_event_wait(g_deadline_event, OVE_WAIT_FOREVER) != OVE_OK)
			continue;
		uint32_t deadline = __atomic_load_n(&g_deadline_count, __ATOMIC_ACQUIRE);
		if (deadline == __atomic_load_n(&g_response_count, __ATOMIC_RELAXED))
			continue; /* drain a stale counting-semaphore post on NuttX */

		uint32_t dispatch_ticks = RT_TIM3_CNT;
		uint32_t missed = deadline - g_last_execution_deadline - 1u;
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
		response_low();
		uint32_t finish_ticks = RT_TIM3_CNT;
		int late_finish = finish_ticks < dispatch_ticks;

		/* Collapse missed deadlines instead of emitting a misleading burst of
		 * late pulses. A new deadline that arrives during the fixed work leaves
		 * its event pending and is handled on the next iteration. */
		__atomic_store_n(&g_response_count, deadline, __ATOMIC_RELAXED);
		uint32_t work_ticks =
			late_finish ? RT_SCOPE_PERIOD_TICKS - dispatch_ticks + finish_ticks
				    : finish_ticks - dispatch_ticks;
		metrics_record(deadline, dispatch_ticks, work_ticks, missed, late_finish);
	}
}

static void metrics_take_window(struct rt_scope_metrics *out)
{
	uint32_t old_active = __atomic_load_n(&g_active_metrics, __ATOMIC_RELAXED);
	uint32_t new_active = old_active ^ 1u;

	/* The reporter is lower priority than the sole metrics writer. Therefore,
	 * if this code is running, no write to the old bucket is in flight. A
	 * response released after the atomic flip writes the other bucket. */
	__atomic_store_n(&g_active_metrics, new_active, __ATOMIC_RELEASE);
	*out = g_metrics[old_active];
	g_metrics[old_active] = (struct rt_scope_metrics){
		.dispatch_min_ticks = UINT32_MAX,
		.work_min_ticks = UINT32_MAX,
	};
}

static char *append_text(char *p, const char *text)
{
	while (*text != '\0')
		*p++ = *text++;
	return p;
}

static char *append_u32(char *p, uint32_t value)
{
	char reversed[10];
	uint32_t count = 0u;

	if (value == 0u) {
		*p++ = '0';
		return p;
	}
	while (value != 0u) {
		reversed[count++] = (char)('0' + value % 10u);
		value /= 10u;
	}
	while (count != 0u)
		*p++ = reversed[--count];
	return p;
}

static char *append_ticks_us(char *p, uint32_t ticks)
{
	uint32_t hundredths = (uint32_t)(((uint64_t)ticks * 100u + RT_SCOPE_TICKS_PER_US / 2u) /
					 RT_SCOPE_TICKS_PER_US);

	p = append_u32(p, hundredths / 100u);
	*p++ = '.';
	*p++ = (char)('0' + (hundredths / 10u) % 10u);
	*p++ = (char)('0' + hundredths % 10u);
	return p;
}

#if defined(CONFIG_OVE_RTOS_FREERTOS)
static char *append_cycles_us(char *p, uint32_t cycles, uint32_t counter_hz)
{
	uint32_t hundredths =
		(uint32_t)(((uint64_t)cycles * 100000000u + counter_hz / 2u) / counter_hz);

	p = append_u32(p, hundredths / 100u);
	*p++ = '.';
	*p++ = (char)('0' + (hundredths / 10u) % 10u);
	*p++ = (char)('0' + hundredths % 10u);
	return p;
}

static const char *svc_syscall_name(uint32_t nr)
{
#define SVC_NAME(name)      \
	case LXP_NR_##name: \
		return #name
	switch (nr) {
		SVC_NAME(exit);
		SVC_NAME(exit_group);
		SVC_NAME(fork);
		SVC_NAME(vfork);
		SVC_NAME(clone);
		SVC_NAME(read);
		SVC_NAME(write);
		SVC_NAME(writev);
		SVC_NAME(pread64);
		SVC_NAME(pwrite64);
		SVC_NAME(open);
		SVC_NAME(openat);
		SVC_NAME(close);
		SVC_NAME(ioctl);
		SVC_NAME(brk);
		SVC_NAME(mmap2);
		SVC_NAME(munmap);
		SVC_NAME(mprotect);
		SVC_NAME(getpid);
		SVC_NAME(getppid);
		SVC_NAME(gettid);
		SVC_NAME(sched_yield);
		SVC_NAME(futex);
		SVC_NAME(futex_time64);
		SVC_NAME(nanosleep);
		SVC_NAME(clock_gettime);
		SVC_NAME(clock_gettime64);
		SVC_NAME(gettimeofday);
		SVC_NAME(poll);
		SVC_NAME(ppoll_time64);
		SVC_NAME(pselect6_time64);
		SVC_NAME(rt_sigaction);
		SVC_NAME(rt_sigprocmask);
		SVC_NAME(rt_sigsuspend);
		SVC_NAME(sigreturn);
		SVC_NAME(rt_sigreturn);
		SVC_NAME(kill);
		SVC_NAME(tkill);
		SVC_NAME(tgkill);
		SVC_NAME(socket);
		SVC_NAME(bind);
		SVC_NAME(connect);
		SVC_NAME(listen);
		SVC_NAME(accept);
		SVC_NAME(accept4);
		SVC_NAME(send);
		SVC_NAME(sendto);
		SVC_NAME(sendmsg);
		SVC_NAME(recv);
		SVC_NAME(recvfrom);
		SVC_NAME(recvmsg);
		SVC_NAME(shutdown);
		SVC_NAME(getsockname);
		SVC_NAME(getpeername);
		SVC_NAME(setsockopt);
		SVC_NAME(getsockopt);
	default:
		return "?";
	}
#undef SVC_NAME
}

static char *append_syscall(char *p, uint32_t nr)
{
	p = append_u32(p, nr);
	*p++ = '(';
	p = append_text(p, svc_syscall_name(nr));
	*p++ = ')';
	return p;
}

static void report_svc_metrics(void)
{
	struct ove_freertos_lnx_svc_metrics window;
	struct ove_freertos_lnx_svc_metrics total;
	char line[192];
	char *p;
	uint32_t counter_hz = ove_freertos_lnx_svc_counter_hz();

	ove_freertos_lnx_svc_metrics_take(&window, &total);

	p = append_text(line, "[rt-scope] svc-us window calls=");
	p = append_u32(p, window.calls);
	if (window.calls != 0u) {
		p = append_text(p, " min=");
		p = append_cycles_us(p, window.min_cycles, counter_hz);
		p = append_text(p, " avg=");
		p = append_cycles_us(p, (uint32_t)(window.total_cycles / window.calls), counter_hz);
		p = append_text(p, " max=");
		p = append_cycles_us(p, window.max_cycles, counter_hz);
		p = append_text(p, " syscall=");
		p = append_syscall(p, window.max_syscall);
	}
	*p++ = '\r';
	*p++ = '\n';
	*p = '\0';
	g_report_write(line);

	p = append_text(line, "[rt-scope] svc-total calls=");
	p = append_u32(p, total.calls);
	if (total.calls != 0u) {
		p = append_text(p, " avg-us=");
		p = append_cycles_us(p, (uint32_t)(total.total_cycles / total.calls), counter_hz);
		p = append_text(p, " max-us=");
		p = append_cycles_us(p, total.max_cycles, counter_hz);
		p = append_text(p, " syscall=");
		p = append_syscall(p, total.max_syscall);
	}
	*p++ = '\r';
	*p++ = '\n';
	*p = '\0';
	g_report_write(line);
}
#endif /* CONFIG_OVE_RTOS_FREERTOS */

static uint32_t percentile_upper_us(const struct rt_scope_metrics *metrics, uint32_t per_mille)
{
	uint64_t target = ((uint64_t)metrics->executions * per_mille + 999u) / 1000u;
	uint64_t cumulative = 0u;

	for (uint32_t i = 0; i < RT_SCOPE_HIST_BINS; ++i) {
		cumulative += metrics->dispatch_hist[i];
		if (cumulative >= target)
			return g_hist_upper_us[i];
	}
	return g_hist_upper_us[RT_SCOPE_HIST_BINS - 1u];
}

struct rt_scope_totals {
	uint32_t releases;
	uint32_t executions;
	uint32_t missed;
	uint32_t late_finish;
	uint32_t irq_overrun;
	uint32_t last_execution_deadline;
};

static void totals_snapshot(struct rt_scope_totals *totals)
{
	uint32_t before;
	uint32_t after;

	do {
		before = __atomic_load_n(&g_total_generation, __ATOMIC_ACQUIRE);
		if ((before & 1u) != 0u)
			continue;
		totals->executions = __atomic_load_n(&g_total_executions, __ATOMIC_RELAXED);
		totals->missed = __atomic_load_n(&g_total_missed, __ATOMIC_RELAXED);
		totals->late_finish = __atomic_load_n(&g_total_late_finish, __ATOMIC_RELAXED);
		totals->last_execution_deadline =
			__atomic_load_n(&g_last_execution_deadline, __ATOMIC_RELAXED);
		totals->irq_overrun = __atomic_load_n(&g_irq_overrun_count, __ATOMIC_RELAXED);
		totals->releases = __atomic_load_n(&g_deadline_count, __ATOMIC_ACQUIRE);
		after = __atomic_load_n(&g_total_generation, __ATOMIC_ACQUIRE);
	} while (before != after || (after & 1u) != 0u);
}

static void report_metrics(void)
{
	struct rt_scope_metrics metrics;
	struct rt_scope_totals totals;
	char line[224];
	char *p;
	static uint32_t previous_releases;

	metrics_take_window(&metrics);
	totals_snapshot(&totals);
	if (metrics.executions == 0u)
		return;

	p = append_text(line, "\r\n[rt-scope] window releases=");
	p = append_u32(p, totals.releases - previous_releases);
	p = append_text(p, " exec=");
	p = append_u32(p, metrics.executions);
	p = append_text(p, " missed=");
	p = append_u32(p, metrics.missed);
	p = append_text(p, " late-finish=");
	p = append_u32(p, metrics.late_finish);
	p = append_text(p, " | total releases=");
	p = append_u32(p, totals.releases);
	p = append_text(p, " exec=");
	p = append_u32(p, totals.executions);
	p = append_text(p, " missed=");
	p = append_u32(p, totals.missed);
	p = append_text(p, " late-finish=");
	p = append_u32(p, totals.late_finish);
	p = append_text(p, " irq-overrun=");
	p = append_u32(p, totals.irq_overrun);
	p = append_text(p, " pending=");
	p = append_u32(p, totals.releases - totals.last_execution_deadline);
	*p++ = '\r';
	*p++ = '\n';
	*p = '\0';
	g_report_write(line);
	previous_releases = totals.releases;

	uint32_t average_ticks = (uint32_t)(metrics.dispatch_sum_ticks / metrics.executions);
	uint32_t jitter_ticks = metrics.dispatch_max_ticks - metrics.dispatch_min_ticks;
	p = append_text(line, "[rt-scope] dispatch-us min=");
	p = append_ticks_us(p, metrics.dispatch_min_ticks);
	p = append_text(p, " avg=");
	p = append_ticks_us(p, average_ticks);
	p = append_text(p, " p99<=");
	p = append_u32(p, percentile_upper_us(&metrics, 990u));
	p = append_text(p, " p99.9<=");
	p = append_u32(p, percentile_upper_us(&metrics, 999u));
	p = append_text(p, " max=");
	p = append_ticks_us(p, metrics.dispatch_max_ticks);
	p = append_text(p, " jitter=");
	p = append_ticks_us(p, jitter_ticks);
	*p++ = '\r';
	*p++ = '\n';
	*p = '\0';
	g_report_write(line);

	p = append_text(line, "[rt-scope] work-us min=");
	p = append_ticks_us(p, metrics.work_min_ticks);
	p = append_text(p, " max=");
	p = append_ticks_us(p, metrics.work_max_ticks);
	p = append_text(p, " late-finish=");
	p = append_u32(p, metrics.late_finish);
	*p++ = '\r';
	*p++ = '\n';
	*p = '\0';
	g_report_write(line);

#if defined(CONFIG_OVE_RTOS_FREERTOS)
	report_svc_metrics();
#endif
}

static void report_thread(void *arg)
{
	(void)arg;
	for (;;) {
		ove_time_delay_ms(RT_SCOPE_REPORT_PERIOD_MS);
		report_metrics();
	}
}

static void gpio_timer_prepare(void)
{
	/* Enable GPIOB/GPIOG and TIM3/TIM5, then reset the timers to remove
	 * state left by a bootloader or an engine peripheral driver. */
	RT_RCC_AHB1ENR |= RT_RCC_AHB1ENR_GPIOBEN | RT_RCC_AHB1ENR_GPIOGEN;
	RT_RCC_APB1ENR |= RT_RCC_APB1_TIM3EN | RT_RCC_APB1_TIM5EN;
	(void)RT_RCC_APB1ENR;
	RT_RCC_APB1RSTR |= RT_RCC_APB1_TIM3EN | RT_RCC_APB1_TIM5EN;
	RT_RCC_APB1RSTR &= ~(RT_RCC_APB1_TIM3EN | RT_RCC_APB1_TIM5EN);

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

	/* 54 MHz phase counter, 1 kHz period, 50 us hardware reference pulse. */
	RT_TIM3_CR1 = 0u;
	RT_TIM3_DIER = 0u;
	RT_TIM3_PSC = RT_SCOPE_TIM3_PRESCALER;
	RT_TIM3_ARR = RT_SCOPE_PERIOD_TICKS - 1u;
	RT_TIM3_CCR1 = RT_SCOPE_REFERENCE_HIGH_TICKS;
	RT_TIM3_CCMR1 = RT_TIM_CCMR1_OC1PE | RT_TIM_CCMR1_OC1M_PWM1;
	RT_TIM3_CCER = RT_TIM_CCER_CC1E;
	RT_TIM3_EGR = RT_TIM_EGR_UG;
	RT_TIM3_SR = 0u;

	/* Pinless coarse counter used to distinguish multiple elapsed periods even
	 * when TIM3's single pending update flag has collapsed them. */
	RT_TIM5_CR1 = 0u;
	RT_TIM5_PSC = RT_SCOPE_TIM5_PRESCALER;
	RT_TIM5_ARR = UINT32_MAX;
	RT_TIM5_CNT = 0u;
	RT_TIM5_EGR = RT_TIM_EGR_UG;
	RT_TIM5_CR1 = RT_TIM_CR1_CEN;

	/* Begin one tick before overflow so the first scheduled CH1 rising edge and
	 * update interrupt correspond. Model the previous release one period before
	 * that edge so a delayed first IRQ can recover every elapsed release. */
	RT_TIM3_CNT = RT_SCOPE_PERIOD_TICKS - 1u;
	g_last_deadline_us = RT_TIM5_CNT - RT_SCOPE_PERIOD_US;
}

int linux_rt_scope_start(linux_rt_scope_write_fn write_fn)
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

	g_report_write = write_fn;
	if (g_report_write != NULL) {
		rc = ove_thread_init(&g_report_thread, &g_report_thread_storage, "rt-report",
				     report_thread, NULL, OVE_PRIO_LOW, RT_SCOPE_REPORT_STACK_BYTES,
				     RT_SCOPE_REPORT_STACK_PTR);
		if (rc != OVE_OK) {
			ove_thread_deinit(g_response_thread);
			ove_event_deinit(g_deadline_event);
			return rc;
		}
	}

	rc = irq_prepare();
	if (rc != OVE_OK) {
		if (g_report_write != NULL)
			ove_thread_deinit(g_report_thread);
		ove_thread_deinit(g_response_thread);
		ove_event_deinit(g_deadline_event);
		return rc;
	}

	gpio_timer_prepare();
	rt_scope_irq_enable();
	RT_TIM3_DIER = RT_TIM_DIER_UIE;
	RT_TIM3_CR1 = RT_TIM_CR1_ARPE | RT_TIM_CR1_CEN;
	return OVE_OK;
}

#else /* !CONFIG_OVE_LINUX_RT_SCOPE */

int linux_rt_scope_start(linux_rt_scope_write_fn write_fn)
{
	(void)write_fn;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_LINUX_RT_SCOPE */
