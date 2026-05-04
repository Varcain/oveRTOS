/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PM

#include "ove/hal/hal_pm.h"
#include "ove/log.h"
#include "ove_backend_common.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdatomic.h>

/* Residency time per low-power state.  ove_hal_pm_enter_state() blocks
 * the pm idle poller in the chosen state for this long.  vTaskDelay
 * lets configUSE_TICKLESS_IDLE suspend SysTick for the delay duration,
 * so the actual sleep cost is paid in the chosen pm state, not in
 * ACTIVE — without that, the FreeRTOS idle task's tickless path would
 * fire AFTER our hook returned (with PM already back in ACTIVE) and the
 * stats would mis-attribute all the real sleep time to ACTIVE.
 *
 * 1 s matches the NuttX / Zephyr backends and gives ~2 transitions/sec
 * during a 5 s sensor cycle, while still re-checking the policy
 * frequently enough to honour the 5 s standby and 30 s deep-sleep
 * thresholds. */
#define PM_STATE_RESIDENCY_MS 1000

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	TickType_t ticks = (expected_idle_ms == OVE_WAIT_FOREVER)
				   ? pdMS_TO_TICKS(PM_STATE_RESIDENCY_MS)
				   : pdMS_TO_TICKS(expected_idle_ms);

	(void)state;
	if (ticks == 0)
		ticks = 1;
	vTaskDelay(ticks);
	return OVE_OK;
}

int ove_hal_pm_wake_arm(const struct ove_pm_wake_src *src)
{
	/* GPIO wake sources: configure EXTI line.
	 * On real hardware this would call HAL_GPIO_Init with
	 * interrupt mode.  The portable layer already registered
	 * the GPIO interrupt via ove_gpio_irq_register().
	 */
	(void)src;
	return OVE_OK;
}

int ove_hal_pm_wake_disarm(const struct ove_pm_wake_src *src)
{
	(void)src;
	return OVE_OK;
}

int ove_hal_pm_domain_enable(ove_pm_domain_t domain)
{
	/* Board-specific: enable power rail / clock gate for domain.
	 * Default: no-op until board support is added.
	 */
	(void)domain;
	return OVE_OK;
}

int ove_hal_pm_domain_disable(ove_pm_domain_t domain)
{
	(void)domain;
	return OVE_OK;
}

uint32_t ove_hal_pm_get_next_timeout_ms(void)
{
	/* FreeRTOS does not expose the next-task-wake time to application
	 * threads.  Return OVE_WAIT_FOREVER so enter_state falls back to
	 * PM_STATE_RESIDENCY_MS pacing. */
	return OVE_WAIT_FOREVER;
}

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

/*
 * The weak vApplicationIdleHook stub lives in freertos_hooks.c so every
 * FreeRTOS build links cleanly regardless of whether PM is enabled.
 *
 * Drive the PM state machine from a dedicated low-priority polling
 * thread, mirroring the NuttX / Zephyr backends.  Using
 * vApplicationIdleHook would also work, but only with tickless idle —
 * and tickless puts the actual sleep AFTER our hook returns (with PM
 * already flipped back to ACTIVE), so all the real low-power time gets
 * mis-attributed to ACTIVE in the stats.  A dedicated thread blocking
 * in vTaskDelay(PM_STATE_RESIDENCY_MS) inside enter_state lets the
 * tickless idle suppress ticks during the delay AND keeps PM in the
 * chosen state for the whole interval, so update_stats() on wake sees
 * the right delta.
 *
 * tskIDLE_PRIORITY + 1 sits one step above the kernel idle task so the
 * poller only runs when no real work is ready, but is still pre-empted
 * by every application thread.
 */
#define PM_IDLE_STACK_DEPTH (configMINIMAL_STACK_SIZE * 4)
static StackType_t pm_idle_stack[PM_IDLE_STACK_DEPTH];
static StaticTask_t pm_idle_tcb;
static TaskHandle_t pm_idle_handle;
static atomic_int pm_idle_running;

static void pm_idle_entry(void *arg)
{
	(void)arg;
	while (atomic_load_explicit(&pm_idle_running, memory_order_acquire)) {
		ove_pm_idle_process();
		taskYIELD();
	}
	vTaskDelete(NULL);
}

int ove_hal_pm_setup(void)
{
	if (atomic_load_explicit(&pm_idle_running, memory_order_acquire))
		return OVE_OK;

	atomic_store_explicit(&pm_idle_running, 1, memory_order_release);
	pm_idle_handle = xTaskCreateStatic(pm_idle_entry, "ove_pm_idle", PM_IDLE_STACK_DEPTH, NULL,
					   tskIDLE_PRIORITY + 1, pm_idle_stack, &pm_idle_tcb);
	if (!pm_idle_handle) {
		atomic_store_explicit(&pm_idle_running, 0, memory_order_release);
		OVE_LOG_ERR("pm: failed to spawn idle thread");
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

void ove_hal_pm_teardown(void)
{
	if (!atomic_load_explicit(&pm_idle_running, memory_order_acquire))
		return;
	atomic_store_explicit(&pm_idle_running, 0, memory_order_release);
	/* Thread self-deletes on exit; no join API. */
}

#endif /* CONFIG_OVE_PM */
