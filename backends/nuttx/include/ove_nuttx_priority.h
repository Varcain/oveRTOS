/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NUTTX_PRIORITY_H
#define OVE_NUTTX_PRIORITY_H

#include "ove/thread.h"

#include <nuttx/config.h>

/* NuttX SCHED_FIFO uses larger numbers for higher priorities. The named ove
 * priorities keep fixed native anchors; the priorities between two anchors
 * spread evenly across the native levels between them, so all 32 stay
 * distinct (the narrowest gap, IDLE..LOW, spaces them two or three apart).
 * Keep the portable critical band above NuttX's high-priority kernel work
 * queue (normally 224): HPWORK is a device-driver bottom half and must not
 * preempt an OVE_PRIO_CRITICAL host task. */
#define OVE_NUTTX_PRIO_IDLE 50
#define OVE_NUTTX_PRIO_LOW 60
#define OVE_NUTTX_PRIO_BELOW_NORMAL 80
#define OVE_NUTTX_PRIO_NORMAL 100
#define OVE_NUTTX_PRIO_ABOVE_NORMAL 120
#define OVE_NUTTX_PRIO_HIGH 150
#define OVE_NUTTX_PRIO_REALTIME 200
#define OVE_NUTTX_PRIO_CRITICAL 240

#if defined(CONFIG_SCHED_HPWORKPRIORITY)
_Static_assert(OVE_NUTTX_PRIO_CRITICAL > CONFIG_SCHED_HPWORKPRIORITY,
	       "OVE_PRIO_CRITICAL must outrank NuttX HPWORK");
#endif

static inline int ove_nuttx_map_priority(ove_prio_t prio)
{
	static const unsigned char ove_at[] = {
		OVE_PRIO_IDLE,	       OVE_PRIO_LOW,  OVE_PRIO_BELOW_NORMAL, OVE_PRIO_NORMAL,
		OVE_PRIO_ABOVE_NORMAL, OVE_PRIO_HIGH, OVE_PRIO_REALTIME,     OVE_PRIO_CRITICAL,
	};
	static const unsigned char native_at[] = {
		OVE_NUTTX_PRIO_IDLE,	 OVE_NUTTX_PRIO_LOW,	      OVE_NUTTX_PRIO_BELOW_NORMAL,
		OVE_NUTTX_PRIO_NORMAL,	 OVE_NUTTX_PRIO_ABOVE_NORMAL, OVE_NUTTX_PRIO_HIGH,
		OVE_NUTTX_PRIO_REALTIME, OVE_NUTTX_PRIO_CRITICAL,
	};
	unsigned p = (unsigned)prio;

	if (p > OVE_PRIO_MAX)
		return OVE_NUTTX_PRIO_NORMAL;
	unsigned i = 1;
	while (p > ove_at[i])
		i++;
	return native_at[i - 1] + (int)((native_at[i] - native_at[i - 1]) * (p - ove_at[i - 1]) /
					(ove_at[i] - ove_at[i - 1]));
}

/* The highest ove priority whose native priority is at or below @p native
 * (ove_thread_list): exact for every priority the map produces. */
static inline ove_prio_t ove_nuttx_unmap_priority(int native)
{
	for (int p = OVE_PRIO_MAX; p > OVE_PRIO_IDLE; p--) {
		if (ove_nuttx_map_priority((ove_prio_t)p) <= native)
			return (ove_prio_t)p;
	}
	return OVE_PRIO_IDLE;
}

#endif /* OVE_NUTTX_PRIORITY_H */
