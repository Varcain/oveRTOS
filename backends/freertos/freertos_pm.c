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

#ifndef __WFI
#define __WFI() __asm volatile ("wfi" ::: "memory")
#endif

int ove_hal_pm_enter_state(ove_pm_state_t state, uint32_t expected_idle_ms)
{
	TickType_t expected_ticks;

	(void)state;

	if (expected_idle_ms == OVE_WAIT_FOREVER)
		expected_ticks = portMAX_DELAY;
	else
		expected_ticks = pdMS_TO_TICKS(expected_idle_ms);

	switch (state) {
	case OVE_PM_STATE_IDLE:
		/* Light sleep — WFI in the idle task context.
		 * FreeRTOS tickless idle suppresses tick interrupts.
		 */
#if configUSE_TICKLESS_IDLE
		vPortSuppressTicksAndSleep(expected_ticks);
#else
		__WFI();
		(void)expected_ticks;
#endif
		break;

	case OVE_PM_STATE_STANDBY:
		/* Platform-specific stop mode.  Board-level code should
		 * override this with actual stop-mode entry (e.g.
		 * HAL_PWR_EnterSTOPMode on STM32).  Default: WFI.
		 */
#if configUSE_TICKLESS_IDLE
		vPortSuppressTicksAndSleep(expected_ticks);
#else
		__WFI();
		(void)expected_ticks;
#endif
		break;

	case OVE_PM_STATE_DEEP_SLEEP:
		/* Platform-specific standby/shutdown.  Default: WFI
		 * until board support provides deeper sleep entry.
		 */
#if configUSE_TICKLESS_IDLE
		vPortSuppressTicksAndSleep(expected_ticks);
#else
		__WFI();
		(void)expected_ticks;
#endif
		break;

	default:
		break;
	}

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
	TickType_t next = xTaskGetTickCount();

	/* FreeRTOS does not expose a direct "next wakeup" API.
	 * eTaskConfirmSleepModeStatus() can confirm sleep is safe.
	 * For now, return WAIT_FOREVER and let the policy decide.
	 */
	(void)next;
	return OVE_WAIT_FOREVER;
}

/*
 * vApplicationIdleHook — called from the FreeRTOS idle task on every
 * iteration.  Drives the PM state machine.
 *
 * Note: if the application already defines vApplicationIdleHook, this
 * will conflict.  In that case the application's hook should call
 * ove_pm_idle_process() explicitly.
 */
#if configUSE_IDLE_HOOK
__attribute__((weak)) void vApplicationIdleHook(void)
{
	ove_pm_idle_process();
}
#endif

void ove_hal_pm_idle_hook(void)
{
	ove_pm_idle_process();
}

#endif /* CONFIG_OVE_PM */
