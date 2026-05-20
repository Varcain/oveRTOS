/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/eventgroup.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include "ove_ns_to_ticks.h"
#include <errno.h>

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_eventgroup_init(ove_eventgroup_t *eg, ove_eventgroup_storage_t *storage)
{
	if (eg == NULL || storage == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	nxsem_init(&storage->waiter, 0, 0);
	storage->bits = 0;
	storage->nwaiters = 0;
	storage->notify_cb = NULL;
	storage->notify_ud = NULL;

	*eg = storage;
	return OVE_OK;
}

void ove_eventgroup_deinit(ove_eventgroup_t eg)
{
	if (eg != NULL) {
		struct ove_eventgroup *g = eg;
		nxsem_destroy(&g->waiter);
	}
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_EVENTGROUP
int ove_eventgroup_create(ove_eventgroup_t *eg)
{
	if (eg == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	struct ove_eventgroup *g = OVE_BACKEND_MALLOC(sizeof(*g));
	if (g == NULL) {
		return OVE_ERR_NO_MEMORY;
	}

	int ret = ove_eventgroup_init(eg, g);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(g);
	}
	return ret;
}

void ove_eventgroup_destroy(ove_eventgroup_t eg)
{
	if (eg != NULL) {
		ove_eventgroup_deinit(eg);
		OVE_BACKEND_FREE(eg);
	}
}
#endif /* OVE_HEAP_EVENTGROUP */

/* ─── Operations ─────────────────────────────────────────────────────── */

ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct ove_eventgroup *g = eg;
	ove_eventbits_t result;
	irqstate_t flags;
	int i;

	flags = enter_critical_section();
	g->bits |= bits;
	result = g->bits;
	for (i = 0; i < g->nwaiters; i++) {
		nxsem_post(&g->waiter);
	}
	ove_notify_cb notify_cb = (bits != 0) ? g->notify_cb : NULL;
	void *notify_ud = g->notify_ud;
	leave_critical_section(flags);

	if (notify_cb) {
		notify_cb(notify_ud);
	}
	return result;
}

ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct ove_eventgroup *g = eg;
	ove_eventbits_t prev;
	irqstate_t flags;

	flags = enter_critical_section();
	prev = g->bits;
	g->bits &= ~bits;
	leave_critical_section(flags);

	return prev;
}

int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint64_t timeout_ns, ove_eventbits_t *result)
{
	struct ove_eventgroup *g = eg;
	int wait_all = (flags & OVE_EG_WAIT_ALL) ? 1 : 0;
	int clear = (flags & OVE_EG_CLEAR_ON_EXIT) ? 1 : 0;
	int matched = 0;
	int ret = 0;
	uint32_t remaining_ticks = 0;
	clock_t deadline = 0;
	irqstate_t irqflags;

	if (timeout_ns != OVE_WAIT_FOREVER) {
		remaining_ticks = ove_ns_to_ticks(timeout_ns);
		deadline = clock_systime_ticks() + remaining_ticks;
	}

	irqflags = enter_critical_section();

	for (;;) {
		if (wait_all) {
			matched = ((g->bits & bits) == bits);
		} else {
			matched = ((g->bits & bits) != 0);
		}

		if (matched) {
			break;
		}

		g->nwaiters++;
		leave_critical_section(irqflags);

		if (timeout_ns == OVE_WAIT_FOREVER) {
			ret = nxsem_wait_uninterruptible(&g->waiter);
		} else {
			clock_t now = clock_systime_ticks();
			if (clock_compare(deadline, now)) {
				ret = -ETIMEDOUT;
			} else {
				ret = nxsem_tickwait_uninterruptible(&g->waiter,
								     (uint32_t)(deadline - now));
			}
		}

		irqflags = enter_critical_section();
		g->nwaiters--;

		if (ret < 0) {
			break;
		}
	}

	if (result != NULL) {
		*result = g->bits;
	}

	if (matched && clear) {
		g->bits &= ~bits;
	}

	leave_critical_section(irqflags);

	return matched ? OVE_OK : OVE_ERR_TIMEOUT;
}

ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	return ove_eventgroup_set_bits(eg, bits);
}

ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	struct ove_eventgroup *g = eg;

	/* Lock-free: 32-bit aligned read is atomic on ARM */
	return g->bits;
}

int ove_eventgroup_set_notify(ove_eventgroup_t eg, ove_notify_cb cb, void *user_data)
{
	struct ove_eventgroup *g = eg;
	if (g == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	irqstate_t flags = enter_critical_section();
	g->notify_cb = cb;
	g->notify_ud = user_data;
	leave_critical_section(flags);
	return OVE_OK;
}
