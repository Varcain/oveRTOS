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

/* NuttX SCHED_FIFO uses larger numbers for higher priorities. Keep the
 * portable critical band above NuttX's high-priority kernel work queue
 * (normally 224): HPWORK is a device-driver bottom half and must not preempt
 * an OVE_PRIO_CRITICAL host task. */
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
	switch (prio) {
	case OVE_PRIO_IDLE:
		return OVE_NUTTX_PRIO_IDLE;
	case OVE_PRIO_LOW:
		return OVE_NUTTX_PRIO_LOW;
	case OVE_PRIO_BELOW_NORMAL:
		return OVE_NUTTX_PRIO_BELOW_NORMAL;
	case OVE_PRIO_NORMAL:
		return OVE_NUTTX_PRIO_NORMAL;
	case OVE_PRIO_ABOVE_NORMAL:
		return OVE_NUTTX_PRIO_ABOVE_NORMAL;
	case OVE_PRIO_HIGH:
		return OVE_NUTTX_PRIO_HIGH;
	case OVE_PRIO_REALTIME:
		return OVE_NUTTX_PRIO_REALTIME;
	case OVE_PRIO_CRITICAL:
		return OVE_NUTTX_PRIO_CRITICAL;
	default:
		return OVE_NUTTX_PRIO_NORMAL;
	}
}

#endif /* OVE_NUTTX_PRIORITY_H */
