/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "model.h"
#include "ove/irq.h"
#include <stdio.h>
#include <string.h>

struct model_state model;
unsigned reads[7], effects, early;
static unsigned failures, queries;
uint32_t model_control(void)
{
	return model.control;
}
uint32_t model_primask(void)
{
	return model.primask;
}
uint32_t model_basepri(void)
{
	return model.basepri;
}
uint32_t model_faultmask(void)
{
	return model.faultmask;
}
unsigned model_effect(void)
{
	++effects;
	return 0;
}

#if defined(MODEL_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"
static unsigned task, idle;
int xPortIsInsideInterrupt(void)
{
	++reads[0];
	return model.isr;
}
#if INCLUDE_xTaskGetSchedulerState == 1
BaseType_t xTaskGetSchedulerState(void)
{
	++reads[1];
#if configNUMBER_OF_CORES > 1
	model_effect();
#endif
	return model.scheduler;
}
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	++reads[2];
	if (model.scheduler != taskSCHEDULER_RUNNING)
		++early;
#if configNUMBER_OF_CORES > 1
	model_effect();
#endif
	return model.idle_current ? &idle : &task;
}
#if configNUMBER_OF_CORES == 1 && INCLUDE_xTaskGetIdleTaskHandle == 1
TaskHandle_t xTaskGetIdleTaskHandle(void)
{
	++reads[3];
	if (model.scheduler != taskSCHEDULER_RUNNING)
		++early;
	return &idle;
}
#endif
#elif defined(MODEL_ZEPHYR)
#include <zephyr/kernel.h>
static struct k_thread task;
struct k_thread z_idle_threads[2];
bool k_is_user_context(void)
{
	++reads[0];
	return model.user;
}
bool k_is_pre_kernel(void)
{
	++reads[1];
	return model.pre_kernel;
}
bool k_is_in_isr(void)
{
	++reads[2];
	return model.isr;
}
struct k_thread *k_current_get(void)
{
	++reads[3];
	if (model.pre_kernel)
		++early;
	if (model.null_current)
		return NULL;
#ifdef CONFIG_SMP
	return model.idle_current ? &z_idle_threads[1] : &task;
#else
	return model.idle_current ? &z_idle_threads[0] : &task;
#endif
}
uint32_t hw_irq_ctrl_get_current_lock(void)
{
	++reads[4];
	return model.native_mask;
}
#elif defined(MODEL_NUTTX)
#include <nuttx/init.h>
#include <nuttx/sched.h>
static struct tcb_s task;
unsigned g_nx_initstate;
bool up_interrupt_context(void)
{
	++reads[0];
	return model.isr;
}
uint64_t up_irq_flags(void)
{
	++reads[2];
	return model.native_mask;
}
struct tcb_s *nxsched_self(void)
{
	++reads[1];
	if (g_nx_initstate != OSINIT_OSREADY && g_nx_initstate != OSINIT_IDLELOOP)
		++early;
	return model.null_current ? NULL : &task;
}
#endif

#ifdef MODEL_PROVIDER_SOURCE
#include MODEL_PROVIDER_SOURCE
#endif

/* Explicit fields exclude padding and keep getter instrumentation separate. */
static void snapshot(uint64_t out[24])
{
	uint64_t state[24] = {model.control,
			      model.primask,
			      model.basepri,
			      model.faultmask,
			      model.isr,
			      model.user,
			      model.pre_kernel,
			      model.scheduler,
			      model.native_mask,
			      model.null_current,
			      model.idle_current,
			      (uint64_t)model.priority,
			      (uint64_t)model.lockcount};
#ifdef MODEL_ZEPHYR
	state[13] = (uint64_t)task.base.priority;
	state[14] = (uint64_t)task.base.sched_locked;
	state[18] = (uint64_t)z_idle_threads[0].base.priority;
	state[19] = (uint64_t)z_idle_threads[0].base.sched_locked;
	state[20] = (uint64_t)z_idle_threads[1].base.priority;
	state[21] = (uint64_t)z_idle_threads[1].base.sched_locked;
#ifdef CONFIG_SMP
	state[15] = task.base.is_idle;
	state[16] = z_idle_threads[0].base.is_idle;
	state[17] = z_idle_threads[1].base.is_idle;
#endif
#elif defined(MODEL_NUTTX)
	state[13] = (uint64_t)task.pid;
	state[14] = (uint64_t)task.lockcount;
	state[15] = g_nx_initstate;
#elif defined(MODEL_FREERTOS)
	state[13] = task;
	state[14] = idle;
#elif defined(MODEL_POSIX)
	state[13] = (uint64_t)s_in_isr;
#ifdef CONFIG_OVE_ASYNC
	state[14] = s_lock_depth;
	state[15] = s_irq_mtx;
	state[16] = s_irq_mtx_once;
#endif
#endif
	memcpy(out, state, sizeof(state));
}

static bool setup(const char *name)
{
	memset(&model, 0, sizeof(model));
	model.scheduler = 2;
	memset(reads, 0, sizeof(reads));
	effects = early = 0;
	if (strstr(name, "__restored_valid"))
		name = "ordinary";
	if (strstr(name, "prestart"))
		model.scheduler = 1;
	if (strstr(name, "isr") && strcmp(name, "after_leave_isr"))
		model.isr = 1;
	if (strstr(name, "unprivileged"))
		model.control = 1;
	if (strstr(name, "control_2"))
		model.control = 2;
	if (strstr(name, "primask") || strstr(name, "masked"))
		model.primask = 1;
	if (strstr(name, "basepri"))
		model.basepri = 0x80;
	if (strstr(name, "faultmask"))
		model.faultmask = 1;
	if (!strcmp(name, "scheduler_suspended"))
		model.scheduler = 0;
	if (!strcmp(name, "user"))
		model.user = 1;
	if (!strcmp(name, "pre_kernel"))
		model.pre_kernel = 1;
	if (!strcmp(name, "null_current"))
		model.null_current = true;
	if (!strcmp(name, "idle_current") || !strcmp(name, "current_idle"))
		model.idle_current = true;
	if (strstr(name, "native_lock") || !strcmp(name, "signal_mask_low"))
		model.native_mask = 1;
	if (!strcmp(name, "signal_mask_high_1ull_shift_40"))
		model.native_mask = UINT64_C(1) << 40;
	if (!strcmp(name, "cooperative_prio_minus_1"))
		model.priority = -1;
	if (!strcmp(name, "scheduler_locked_255"))
		model.lockcount = 255;
#ifdef MODEL_ZEPHYR
	memset(&task, 0, sizeof(task));
	memset(z_idle_threads, 0, sizeof(z_idle_threads));
	task.base.priority = model.priority;
	task.base.sched_locked = model.lockcount;
#ifdef CONFIG_SMP
	z_idle_threads[0].base.is_idle = z_idle_threads[1].base.is_idle = true;
#endif
#elif defined(MODEL_NUTTX)
	g_nx_initstate = OSINIT_OSREADY;
	const char *states[] = {"powerup",  "boot",    "tasklists", "memory",
				"hardware", "osready", "idleloop",  "panic"};
	for (unsigned i = 0; i < 8; ++i)
		if (!strcmp(name, states[i]))
			g_nx_initstate = i;
	task.pid = !strcmp(name, "idle_pid") ? IDLE_PROCESS_ID : 1;
	task.lockcount = !strcmp(name, "lockcount_1") ? 1 : 0;
#elif defined(MODEL_POSIX)
	s_in_isr = model.isr;
	if (!strcmp(name, "after_leave_isr")) {
		posix_irq_enter_isr();
		posix_irq_leave_isr();
	}
#ifdef CONFIG_OVE_ASYNC
	s_lock_depth = 0;
	if (!strcmp(name, "depth_1") || !strcmp(name, "isr_and_depth"))
		s_lock_depth = 1;
	if (!strcmp(name, "depth_2"))
		s_lock_depth = 2;
	if (!strcmp(name, "depth_1ull_shift_32"))
		s_lock_depth = UINT64_C(1) << 32;
#endif
#endif
	return true;
}

static void check(bool valid, const char *message)
{
	if (!valid) {
		++failures;
		printf("failed: %s\n", message);
	}
}
static void observe(const char *name, uint32_t expected, const unsigned wanted[7]);
int main(int argc, char **argv)
{
	if (argc != 1 && (argc != 2 || strcmp(argv[1], "canary")))
		return 2;
	check(OVE_SYNC_CONTEXT_RESOURCE == 1 && OVE_SYNC_CONTEXT_BLOCKING == 2, "public constants");
	if (argc == 2)
		check(false, "explicit early failure canary");
#include "cases.inc"
	printf("context-model: profile=%s failures=%u queries=%u\n", MODEL_PROFILE, failures,
	       queries);
	return failures ? 1 : 0;
}

static void observe(const char *name, uint32_t expected, const unsigned wanted[7])
{
	uint64_t before[24], after[24];
	setup(name);
	snapshot(before);
	uint32_t flags = ove_sync_context();
	snapshot(after);
	check(flags == expected, "context flags");
	check(!memcmp(before, after, sizeof(before)), "query preserves metadata");
	check(effects == 0 && early == 0, "query avoids native effects and early getters");
	check(!memcmp(reads, wanted, sizeof(reads)), "query getter counts");
	++queries;
	printf("%s: flags=%u readonly=%u effects=%u early=%u getters=%u,%u,%u,%u,%u,%u,%u\n", name,
	       flags, (unsigned)!memcmp(before, after, sizeof(before)), effects, early, reads[0],
	       reads[1], reads[2], reads[3], reads[4], reads[5], reads[6]);
}
