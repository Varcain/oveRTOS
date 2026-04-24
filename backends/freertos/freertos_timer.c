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

int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage,
		       ove_timer_fn callback, void *user_data,
		       uint32_t period_ms, int one_shot)
{
	if (timer == NULL || storage == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->callback = callback;
	storage->user_data = user_data;

	storage->handle = xTimerCreateStatic("ove",
				   pdMS_TO_TICKS(period_ms),
				   one_shot ? pdFALSE : pdTRUE,
				   (void *)storage,
				   freertos_timer_callback,
				   &storage->static_timer);

	*timer = storage;
	return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
	if (timer != NULL) {
		xTimerDelete(timer->handle, 0);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_TIMER
int ove_timer_create(ove_timer_t *timer,
				 ove_timer_fn callback,
				 void *user_data, uint32_t period_ms,
				 int one_shot)
{
	struct ove_timer *ctx;

	if (timer == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (ctx == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_timer_init(timer, ctx, callback, user_data,
				     period_ms, one_shot);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(ctx);
	}
	return ret;
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
