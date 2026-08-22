/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Native NuttX timer backend.
 *
 * A watchdog (wd_start/wd_cancel) drives the period in timer-interrupt
 * context and defers the user callback to the high-priority work queue
 * (HPWORK) so the callback runs in thread context — safe for mutexes and
 * LVGL.  This bypasses the POSIX timer pool + SIGEV_THREAD layer (lower
 * runtime cost) and, crucially, lets teardown drain a running callback via
 * work_cancel_sync() instead of a best-effort sleep — closing the
 * use-after-free window where a deferred callback could outlive free().
 *
 * Requires CONFIG_SCHED_HPWORK=y (already implied by the prior
 * CONFIG_SIG_EVTHREAD_HPWORK path).
 */

#include "ove/timer.h"
#include "ove/storage.h"
#include "ove_ns_to_ticks.h"
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>
#include <string.h>

#define OVE_TIMER_WORKQUEUE HPWORK

/* Thread-context half of a firing: runs the user callback. */
static void timer_work_handler(void *arg)
{
	struct ove_timer *ctx = arg;
	if (ctx != NULL && ctx->callback != NULL) {
		ctx->callback(ctx, ctx->user_data);
	}
}

/* IRQ-context half of a firing: re-arm (periodic) and defer the callback. */
static void timer_wd_expiry(wdparm_t arg)
{
	struct ove_timer *ctx = (struct ove_timer *)arg;

	/* Re-arm before queuing the work so the period isn't skewed by the
	 * callback's run time. */
	if (!ctx->one_shot) {
		clock_t ticks = ove_ns_to_ticks(ctx->period_ns);
		if (ticks == 0) {
			ticks = 1;
		}
		wd_start(&ctx->wdog, ticks, timer_wd_expiry, (wdparm_t)ctx);
	}

	work_queue(OVE_TIMER_WORKQUEUE, &ctx->work, timer_work_handler, ctx, 0);
}

static int timer_setup(struct ove_timer *ctx, ove_timer_fn callback, void *user_data,
		       uint64_t period_ns, int one_shot)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->callback = callback;
	ctx->user_data = user_data;
	ctx->period_ns = period_ns;
	ctx->one_shot = one_shot;
	return OVE_OK;
}

/* Stop future firings, then block until any callback already running on the
 * work queue has returned — so the caller may safely free the storage. */
static void timer_cleanup(struct ove_timer *ctx)
{
	wd_cancel(&ctx->wdog);
	ctx->callback = NULL;
	work_cancel_sync(OVE_TIMER_WORKQUEUE, &ctx->work);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_timer_init_ns(ove_timer_t *timer, ove_timer_storage_t *storage, ove_timer_fn callback,
		      void *user_data, uint64_t period_ns, int one_shot)
{
	if (timer == NULL || storage == NULL || callback == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	timer_setup(storage, callback, user_data, period_ns, one_shot);
	*timer = storage;
	return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
	if (timer != NULL) {
		timer_cleanup(timer);
	}
}

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_timer_start(ove_timer_t timer)
{
	DEBUGASSERT(timer != NULL);
	struct ove_timer *ctx = timer;

	clock_t ticks = ove_ns_to_ticks(ctx->period_ns);
	/* A zero-tick delay would fire immediately / be rejected; clamp so a
	 * sub-tick period still arms.  The Embassy time driver never schedules
	 * zero-delay deadlines, but this also guards stale reprogram state. */
	if (ticks == 0) {
		ticks = 1;
	}

	int ret = wd_start(&ctx->wdog, ticks, timer_wd_expiry, (wdparm_t)ctx);
	return ret == 0 ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_timer_stop(ove_timer_t timer)
{
	DEBUGASSERT(timer != NULL);
	struct ove_timer *ctx = timer;

	/* Stop future firings.  A callback already deferred to the work queue
	 * may still run once after this returns (matches the prior behavior and
	 * the test's "<= +1 after stop" tolerance); teardown's
	 * work_cancel_sync() is what guarantees no callback survives free. */
	wd_cancel(&ctx->wdog);
	return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
	DEBUGASSERT(timer != NULL);
	ove_timer_stop(timer);
	return ove_timer_start(timer);
}

int ove_timer_set_period_ns(ove_timer_t timer, uint64_t period_ns)
{
	struct ove_timer *ctx = timer;
	if (ctx == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	ctx->period_ns = period_ns;
	return ove_timer_start(ctx);
}
