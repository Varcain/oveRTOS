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
 * FreeRTOS tick (typically 1 ms) is the resolution floor; round up, to
 * whole milliseconds and then to whole ticks, so the timer never fires
 * earlier than the caller requested,
 * and clamp to 1 so a zero-tick (= disabled) timer is never created.  A
 * period whose tick count does not fit TickType_t (about 49.7 days at a
 * 1 kHz tick) is refused: truncating it would arm a shorter period. */
static int ns_to_ticks_ceil(uint64_t period_ns, TickType_t *ticks)
{
	const uint64_t period_ms = period_ns / 1000000ULL + (period_ns % 1000000ULL != 0);
	const uint64_t t = (period_ms * (uint64_t)configTICK_RATE_HZ + 999ULL) / 1000ULL;

	if (t >= (uint64_t)portMAX_DELAY) {
		return OVE_ERR_INVALID_PARAM;
	}
	*ticks = t == 0 ? 1 : (TickType_t)t;
	return OVE_OK;
}

/* Callable from a task or an ISR, as ove_timer_set_period_ns is. */
static void store_period(ove_timer_t timer, uint64_t period_ns)
{
	UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
	timer->period_ns_lo = (uint32_t)period_ns;
	timer->period_ns_hi = (uint32_t)(period_ns >> 32);
	taskEXIT_CRITICAL_FROM_ISR(mask);
}

int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot)
{
	if (timer == NULL || storage == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	TickType_t ticks;
	if (ns_to_ticks_ceil(period_ns, &ticks) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->callback = callback;
	storage->user_data = user_data;
	store_period(storage, period_ns);

	storage->handle = xTimerCreateStatic("ove", ticks, one_shot ? pdFALSE : pdTRUE,
					     (void *)storage, freertos_timer_callback,
					     &storage->static_timer);

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
	 * ISR context, dispatch to xTimerChangePeriodFromISR.  A period that is
	 * refused leaves the timer as it was. */
	TickType_t ticks;
	if (ns_to_ticks_ceil(period_ns, &ticks) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	if (xPortIsInsideInterrupt()) {
		BaseType_t woken = pdFALSE;
		if (xTimerChangePeriodFromISR(timer->handle, ticks, &woken) != pdPASS) {
			return OVE_ERR_TIMEOUT;
		}
		portYIELD_FROM_ISR(woken);
	} else {
		if (xTimerChangePeriod(timer->handle, ticks, 0) != pdPASS) {
			return OVE_ERR_TIMEOUT;
		}
	}
	store_period(timer, period_ns);
	return OVE_OK;
}

uint64_t ove_timer_get_period_ns(ove_timer_t timer)
{
	if (timer == NULL) {
		return 0;
	}
	UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
	uint64_t period_ns = ((uint64_t)timer->period_ns_hi << 32) | timer->period_ns_lo;
	taskEXIT_CRITICAL_FROM_ISR(mask);
	return period_ns;
}
