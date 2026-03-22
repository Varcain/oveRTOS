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
#include <zephyr/kernel.h>

/* k_timer expiry runs in ISR context — defer to system workqueue */
static void zephyr_timer_expiry(struct k_timer *ztimer)
{
	struct ove_timer *ctx =
		CONTAINER_OF(ztimer, struct ove_timer, timer);
	k_work_submit(&ctx->work);
}

/* Runs in thread context (system workqueue) — safe to lock mutexes */
static void zephyr_timer_work(struct k_work *work)
{
	struct ove_timer *ctx =
		CONTAINER_OF(work, struct ove_timer, work);
	if (ctx->callback != NULL) {
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
	storage->period_ms = period_ms;
	storage->one_shot = one_shot;

	k_timer_init(&storage->timer, zephyr_timer_expiry, NULL);
	k_work_init(&storage->work, zephyr_timer_work);

	*timer = storage;
	return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
	if (timer != NULL) {
		k_timer_stop(&timer->timer);
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
	k_timeout_t period = K_MSEC(timer->period_ms);
	k_timeout_t duration = period;

	if (timer->one_shot) {
		k_timer_start(&timer->timer, duration, K_NO_WAIT);
	} else {
		k_timer_start(&timer->timer, duration, period);
	}
	return OVE_OK;
}

int ove_timer_stop(ove_timer_t timer)
{
	k_timer_stop(&timer->timer);
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	/* Stop and restart with original parameters */
	ove_timer_stop(timer);
	return ove_timer_start(timer);
}
