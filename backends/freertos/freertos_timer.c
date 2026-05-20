/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/timer.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "timers.h"
static void freertos_timer_callback(TimerHandle_t xTimer)
{
	struct ove_timer *ctx = pvTimerGetTimerID(xTimer);
	if (ctx != NULL && ctx->callback != NULL) {
		ctx->callback(ctx, ctx->user_data);
	}
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

int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		   void *user_data, uint32_t period_ms, int one_shot)
{
	return ove_timer_init_ns(timer, storage, callback, user_data,
				 (uint64_t)period_ms * 1000000ULL, one_shot);
}

void ove_timer_deinit(ove_timer_t timer)
{
	if (timer != NULL) {
		xTimerDelete(timer->handle, 0);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_TIMER
int ove_timer_create_ns(ove_timer_t *timer, ove_timer_fn callback, void *user_data,
			uint64_t period_ns, int one_shot)
{
	struct ove_timer *ctx;

	if (timer == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (ctx == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_timer_init_ns(timer, ctx, callback, user_data, period_ns, one_shot);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(ctx);
	}
	return ret;
}

int ove_timer_create(ove_timer_t *timer, ove_timer_fn callback, void *user_data, uint32_t period_ms,
		     int one_shot)
{
	return ove_timer_create_ns(timer, callback, user_data, (uint64_t)period_ms * 1000000ULL,
				   one_shot);
}

void ove_timer_destroy(ove_timer_t timer)
{
	if (timer != NULL) {
		ove_timer_deinit(timer);
		OVE_BACKEND_FREE(timer);
	}
}
#endif /* OVE_HEAP_TIMER */

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
