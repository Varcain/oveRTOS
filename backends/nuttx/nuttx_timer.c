/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * POSIX timer backend using SIGEV_THREAD.
 *
 * On NuttX this requires CONFIG_SIG_EVTHREAD=y.  Callbacks execute in
 * work queue thread context (no signal delivery), safe for mutexes and LVGL.
 * On Linux/POSIX, SIGEV_THREAD is natively supported.
 */

#include "ove/timer.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include <signal.h>
#include <time.h>
#include <string.h>
#ifndef __NuttX__
#include <unistd.h>
#endif

static void timer_thread_handler(union sigval sv)
{
  struct ove_timer *ctx = sv.sival_ptr;
  if (ctx != NULL && ctx->callback != NULL)
    {
      ctx->callback(ctx, ctx->user_data);
    }
}

static int timer_setup(struct ove_timer *ctx,
                       ove_timer_fn callback,
                       void *user_data, uint32_t period_ms,
                       int one_shot)
{
  struct sigevent sev;
  int ret;

  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->period_ms = period_ms;
  ctx->one_shot = one_shot;

  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_THREAD;
  sev.sigev_notify_function = timer_thread_handler;
  sev.sigev_value.sival_ptr = ctx;

  ret = timer_create(CLOCK_MONOTONIC, &sev, &ctx->posix_timer);
  if (ret != 0)
    {
      return OVE_ERR_NOT_SUPPORTED;
    }

  return OVE_OK;
}

static void timer_cleanup(struct ove_timer *ctx)
{
  /* Disarm the timer first */
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  timer_settime(ctx->posix_timer, 0, &its, NULL);

  /* Prevent in-flight callbacks from dereferencing freed memory */
  ctx->callback = NULL;

#ifndef __NuttX__
  /* SIGEV_THREAD: timer_delete does not wait for in-progress callback
   * threads. Brief sleep to let any in-flight callback complete. */
  usleep(2000);
#endif

  timer_delete(ctx->posix_timer);
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_timer_init(ove_timer_t *timer, ove_timer_storage_t *storage,
                       ove_timer_fn callback, void *user_data,
                       uint32_t period_ms, int one_shot)
{
  if (timer == NULL || storage == NULL || callback == NULL)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  int ret = timer_setup(storage, callback, user_data, period_ms, one_shot);
  if (ret != OVE_OK)
    {
      return ret;
    }

  *timer = storage;
  return OVE_OK;
}

void ove_timer_deinit(ove_timer_t timer)
{
  if (timer != NULL)
    {
      timer_cleanup(timer);
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

  if (timer == NULL || callback == NULL)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
  if (ctx == NULL)
    {
      return OVE_ERR_NO_MEMORY;
    }

  int ret = timer_setup(ctx, callback, user_data, period_ms, one_shot);
  if (ret != OVE_OK)
    {
      OVE_BACKEND_FREE(ctx);
      return ret;
    }

  *timer = ctx;
  return OVE_OK;
}

void ove_timer_destroy(ove_timer_t timer)
{
  if (timer != NULL)
    {
      timer_cleanup(timer);
      OVE_BACKEND_FREE(timer);
    }
}
#endif /* OVE_HEAP_TIMER */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_timer_start(ove_timer_t timer)
{
  struct ove_timer *ctx = timer;
  struct itimerspec its;

  its.it_value.tv_sec = ctx->period_ms / 1000;
  its.it_value.tv_nsec = (ctx->period_ms % 1000) * 1000000L;

  if (ctx->one_shot)
    {
      its.it_interval.tv_sec = 0;
      its.it_interval.tv_nsec = 0;
    }
  else
    {
      its.it_interval = its.it_value;
    }

  int ret = timer_settime(ctx->posix_timer, 0, &its, NULL);
  if (ret != 0)
    {
      return OVE_ERR_NOT_SUPPORTED;
    }

  return OVE_OK;
}

int ove_timer_stop(ove_timer_t timer)
{
  struct ove_timer *ctx = timer;
  struct itimerspec its;

  memset(&its, 0, sizeof(its));
  int ret = timer_settime(ctx->posix_timer, 0, &its, NULL);
  if (ret != 0)
    {
      return OVE_ERR_NOT_SUPPORTED;
    }

  return OVE_OK;
}

int ove_timer_reset(ove_timer_t timer)
{
  ove_timer_stop(timer);
  return ove_timer_start(timer);
}
