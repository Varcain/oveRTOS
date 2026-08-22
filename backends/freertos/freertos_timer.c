/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/timer.h"
#include "ove/storage.h"
#include "FreeRTOS.h"
#include "timers.h"
#include "semphr.h"
static void freertos_timer_callback(TimerHandle_t xTimer)
{
	struct ove_timer *ctx = pvTimerGetTimerID(xTimer);
	if (ctx != NULL && ctx->callback != NULL) {
		ctx->callback(ctx, ctx->user_data);
	}
}

/* Pended onto the timer daemon by teardown; gives the drain semaphore. */
static void freertos_timer_drain_done(void *param1, uint32_t param2)
{
	(void)param2;
	xSemaphoreGive((SemaphoreHandle_t)param1);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

/* Convert a nanosecond period to ticks at the kernel's tick rate.  The
 * FreeRTOS tick (typically 1 ms) is the resolution floor; round up so
 * the timer never fires earlier than the caller requested, and clamp
 * to 1 so a zero-tick (= disabled) timer is never created. */
static TickType_t ns_to_ticks_ceil(uint64_t period_ns)
{
	const uint64_t period_us = (period_ns + 999ULL) / 1000ULL;
	const uint64_t period_ms = (period_us + 999ULL) / 1000ULL;
	TickType_t ticks = pdMS_TO_TICKS((uint32_t)period_ms);
	if (ticks == 0) {
		ticks = 1;
	}
	return ticks;
}

int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot)
{
	if (timer == NULL || storage == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->callback = callback;
	storage->user_data = user_data;

	storage->handle = xTimerCreateStatic("ove", ns_to_ticks_ceil(period_ns),
					     one_shot ? pdFALSE : pdTRUE, (void *)storage,
					     freertos_timer_callback, &storage->static_timer);

	*timer = storage;
	return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
	if (timer == NULL) {
		return;
	}

	/* xTimerDelete only queues a delete command — the timer daemon may be
	 * mid-callback (pvTimerGetTimerID -> ctx) or have yet to process the
	 * delete, so freeing the wrapper (which embeds static_timer) here would
	 * be a use-after-free.  Pend a give onto the daemon after the delete:
	 * the daemon runs the callback, the delete, and this give serially on
	 * one task, so taking the semaphore means the timer has fully drained
	 * and the storage is safe to reuse/free.
	 *
	 * Must not be called from the timer's own callback (i.e. the daemon
	 * task) — that would self-deadlock, same constraint as join-from-self. */
	xTimerDelete(timer->handle, portMAX_DELAY);

	StaticSemaphore_t sem_buf;
	SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&sem_buf);
	if (done != NULL &&
	    xTimerPendFunctionCall(freertos_timer_drain_done, done, 0, portMAX_DELAY) == pdPASS) {
		xSemaphoreTake(done, portMAX_DELAY);
		vSemaphoreDelete(done);
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_timer_start(ove_timer_t timer)
{
	configASSERT(timer != NULL);
	if (xTimerStart(timer->handle, 0) != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_timer_stop(ove_timer_t timer)
{
	configASSERT(timer != NULL);
	if (xTimerStop(timer->handle, 0) != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	configASSERT(timer != NULL);
	if (xTimerReset(timer->handle, 0) != pdPASS) {
		return OVE_ERR_TIMEOUT;
	}
	return OVE_OK;
}

int ove_timer_set_period_ns(ove_timer_t timer, uint64_t period_ns)
{
	configASSERT(timer != NULL);
	/* xTimerChangePeriod atomically changes the period and (re)arms the
	 * timer with the new period from the moment of the call.  No stop +
	 * recreate is needed — the kernel-side timer object stays valid.  In
	 * ISR context, dispatch to xTimerChangePeriodFromISR. */
	if (xPortIsInsideInterrupt()) {
		BaseType_t woken = pdFALSE;
		if (xTimerChangePeriodFromISR(timer->handle, ns_to_ticks_ceil(period_ns), &woken) !=
		    pdPASS) {
			return OVE_ERR_TIMEOUT;
		}
		portYIELD_FROM_ISR(woken);
	} else {
		if (xTimerChangePeriod(timer->handle, ns_to_ticks_ceil(period_ns), 0) != pdPASS) {
			return OVE_ERR_TIMEOUT;
		}
	}
	return OVE_OK;
}
