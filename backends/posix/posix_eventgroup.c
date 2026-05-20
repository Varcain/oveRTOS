/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>
static void ns_to_abstime(uint64_t timeout_ns, struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += (time_t)(timeout_ns / 1000000000ULL);
	ts->tv_nsec += (long)(timeout_ns % 1000000000ULL);
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

int ove_eventgroup_init(ove_eventgroup_t *eg, ove_eventgroup_storage_t *storage)
{
	if (!eg || !storage)
		return OVE_ERR_INVALID_PARAM;
	struct ove_eventgroup *g = (struct ove_eventgroup *)storage;
	memset(g, 0, sizeof(*g));
	pthread_mutex_init(&g->lock, NULL);
	pthread_cond_init(&g->cond, NULL);
	*eg = g;
	return OVE_OK;
}

void ove_eventgroup_deinit(ove_eventgroup_t eg)
{
	struct ove_eventgroup *g = eg;
	if (g) {
		pthread_mutex_destroy(&g->lock);
		pthread_cond_destroy(&g->cond);
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_eventgroup_create(ove_eventgroup_t *eg)
{
	if (!eg)
		return OVE_ERR_INVALID_PARAM;
	struct ove_eventgroup *g = OVE_BACKEND_MALLOC(sizeof(*g));
	if (!g) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(g, 0, sizeof(*g));
	pthread_mutex_init(&g->lock, NULL);
	pthread_cond_init(&g->cond, NULL);
	*eg = g;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_eventgroup_destroy(ove_eventgroup_t eg)
{
	struct ove_eventgroup *g = eg;
	if (g) {
		pthread_mutex_destroy(&g->lock);
		pthread_cond_destroy(&g->cond);
		OVE_BACKEND_FREE(g);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct ove_eventgroup *g = eg;
	if (!g) {
		return 0;
	}
	pthread_mutex_lock(&g->lock);
	g->bits |= bits;
	ove_eventbits_t result = g->bits;
	pthread_cond_broadcast(&g->cond);
	/* Snapshot the notify hook under the lock; fire after unlock. */
	ove_notify_cb notify_cb = (bits != 0) ? g->notify_cb : NULL;
	void *notify_ud = g->notify_ud;
	pthread_mutex_unlock(&g->lock);
	if (notify_cb) {
		notify_cb(notify_ud);
	}
	return result;
}

ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct ove_eventgroup *g = eg;
	if (!g) {
		return 0;
	}
	pthread_mutex_lock(&g->lock);
	ove_eventbits_t prev = g->bits;
	g->bits &= ~bits;
	pthread_mutex_unlock(&g->lock);
	return prev;
}

int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint64_t timeout_ns, ove_eventbits_t *result)
{
	struct ove_eventgroup *g = eg;
	if (!g) {
		return OVE_ERR_INVALID_PARAM;
	}
	int wait_all = flags & OVE_EG_WAIT_ALL;
	int clear = flags & OVE_EG_CLEAR_ON_EXIT;

	pthread_mutex_lock(&g->lock);

	struct timespec ts;
	if (timeout_ns != OVE_WAIT_FOREVER) {
		ns_to_abstime(timeout_ns, &ts);
	}

	for (;;) {
		int satisfied;
		if (wait_all) {
			satisfied = (g->bits & bits) == bits;
		} else {
			satisfied = (g->bits & bits) != 0;
		}

		if (satisfied) {
			if (result) {
				*result = g->bits;
			}
			if (clear) {
				g->bits &= ~bits;
			}
			pthread_mutex_unlock(&g->lock);
			return OVE_OK;
		}

		int ret;
		if (timeout_ns == OVE_WAIT_FOREVER) {
			ret = pthread_cond_wait(&g->cond, &g->lock);
		} else {
			ret = pthread_cond_timedwait(&g->cond, &g->lock, &ts);
		}
		if (ret == ETIMEDOUT) {
			if (result) {
				*result = g->bits;
			}
			pthread_mutex_unlock(&g->lock);
			return OVE_ERR_TIMEOUT;
		}
	}
}

ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	return ove_eventgroup_set_bits(eg, bits);
}

ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	struct ove_eventgroup *g = eg;
	if (!g) {
		return 0;
	}
	pthread_mutex_lock(&g->lock);
	ove_eventbits_t result = g->bits;
	pthread_mutex_unlock(&g->lock);
	return result;
}

int ove_eventgroup_set_notify(ove_eventgroup_t eg, ove_notify_cb cb, void *user_data)
{
	struct ove_eventgroup *g = eg;
	if (!g) {
		return OVE_ERR_INVALID_PARAM;
	}
	pthread_mutex_lock(&g->lock);
	g->notify_cb = cb;
	g->notify_ud = user_data;
	pthread_mutex_unlock(&g->lock);
	return OVE_OK;
}
