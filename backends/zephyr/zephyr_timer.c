/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/timer.h"
#include "ove/storage.h"
#include <zephyr/kernel.h>
/* k_timer expiry runs in ISR context — defer to system workqueue */
static void zephyr_timer_expiry(struct k_timer *ztimer)
{
	struct ove_timer *ctx = CONTAINER_OF(ztimer, struct ove_timer, timer);
	k_work_submit(&ctx->work);
}

/* Runs in thread context (system workqueue) — safe to lock mutexes */
static void zephyr_timer_work(struct k_work *work)
{
	struct ove_timer *ctx = CONTAINER_OF(work, struct ove_timer, work);
	if (ctx->callback != NULL) {
		ctx->callback(ctx, ctx->user_data);
	}
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot)
{
	if (timer == NULL || storage == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	storage->callback = callback;
	storage->user_data = user_data;
	storage->period_ns = period_ns;
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
		/* k_timer_stop does not cancel the k_work the expiry submitted;
		 * a pending/running deferred callback would deref freed storage
		 * via CONTAINER_OF (use-after-free).  k_work_cancel_sync()
		 * removes a queued item and blocks until a running one returns,
		 * so the caller may safely free the storage afterwards.  Must
		 * not run on the system workqueue thread (deinit/destroy are
		 * called from app context, never the worker). */
		struct k_work_sync sync;
		k_work_cancel_sync(&timer->work, &sync);
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_timer_start(ove_timer_t timer)
{
	__ASSERT(timer != NULL, "NULL timer handle");
	/* Zephyr k_timeout_t carries native ns resolution via K_NSEC; the
	 * underlying kernel clock determines the actual floor (typically
	 * hardware-tick granularity, often 31 µs at 32 kHz).  Clamp 0 to
	 * 1 ns so a zero-duration deadline doesn't disarm the timer. */
	uint64_t period_ns = timer->period_ns == 0 ? 1 : timer->period_ns;
	k_timeout_t period = K_NSEC(period_ns);
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
	__ASSERT(timer != NULL, "NULL timer handle");
	k_timer_stop(&timer->timer);
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	__ASSERT(timer != NULL, "NULL timer handle");
	ove_timer_stop(timer);
	return ove_timer_start(timer);
}

int ove_timer_set_period_ns(ove_timer_t timer, uint64_t period_ns)
{
	__ASSERT(timer != NULL, "NULL timer handle");
	timer->period_ns = period_ns;
	return ove_timer_start(timer);
}
