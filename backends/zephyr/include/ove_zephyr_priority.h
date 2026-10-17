/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_ZEPHYR_PRIORITY_H
#define OVE_ZEPHYR_PRIORITY_H

#include "ove/thread.h"

#include <zephyr/kernel.h>

/* Zephyr's non-negative priorities are preemptible; lower numbers win. The ove
 * scale maps one to one onto 32 of them (CONFIG_NUM_PREEMPT_PRIORITIES=32 in
 * every build), highest ove priority first. The personality's infrastructure
 * sits at fixed points of the same scale, in this order:
 *
 *   ove  native
 *   31    0   critical        scope response
 *   24    7   realtime
 *   20   11   high
 *   18   13   coordinator     privileged LXP run loop (assigned by the seam)
 *   16   15   above-normal    Ethernet RX
 *   15   16   network TC / guest-quantum server
 *   14   17   guest           unprivileged Linux programs
 *   13   18   system workqueue
 *   12   19   normal          ordinary oveRTOS threads and main
 *    8,4,0    below-normal, low, idle
 *
 * The matching Kconfig values live in config/templates/prj.conf.j2. */
#define OVE_ZEPHYR_NATIVE_PRIORITY(prio) ((int)OVE_PRIO_MAX - (int)(prio))

BUILD_ASSERT(
	CONFIG_NUM_PREEMPT_PRIORITIES > OVE_PRIO_MAX,
	"Zephyr needs a preemptible priority for every ove priority (NUM_PREEMPT_PRIORITIES 32)");

#define OVE_ZEPHYR_PRIO_CRITICAL OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_CRITICAL)
#define OVE_ZEPHYR_PRIO_REALTIME OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_REALTIME)
#define OVE_ZEPHYR_PRIO_HIGH OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_HIGH)
#define OVE_ZEPHYR_PRIO_LXP_COORDINATOR OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_BELOW(OVE_PRIO_HIGH, 2))
#define OVE_ZEPHYR_PRIO_ABOVE_NORMAL OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_ABOVE_NORMAL)
#define OVE_ZEPHYR_PRIO_NET_TC OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_BELOW(OVE_PRIO_ABOVE_NORMAL, 1))
#define OVE_ZEPHYR_PRIO_LXP_GUEST \
	OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_BELOW(OVE_PRIO_ABOVE_NORMAL, 2))
#define OVE_ZEPHYR_PRIO_SYSTEM_WORKQUEUE \
	OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_BELOW(OVE_PRIO_ABOVE_NORMAL, 3))
#define OVE_ZEPHYR_PRIO_NORMAL OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_NORMAL)
#define OVE_ZEPHYR_PRIO_BELOW_NORMAL OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_BELOW_NORMAL)
#define OVE_ZEPHYR_PRIO_LOW OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_LOW)
#define OVE_ZEPHYR_PRIO_IDLE OVE_ZEPHYR_NATIVE_PRIORITY(OVE_PRIO_IDLE)

BUILD_ASSERT(OVE_ZEPHYR_PRIO_HIGH < OVE_ZEPHYR_PRIO_LXP_COORDINATOR &&
		     OVE_ZEPHYR_PRIO_LXP_COORDINATOR < OVE_ZEPHYR_PRIO_ABOVE_NORMAL &&
		     OVE_ZEPHYR_PRIO_ABOVE_NORMAL < OVE_ZEPHYR_PRIO_NET_TC &&
		     OVE_ZEPHYR_PRIO_NET_TC < OVE_ZEPHYR_PRIO_LXP_GUEST &&
		     OVE_ZEPHYR_PRIO_LXP_GUEST < OVE_ZEPHYR_PRIO_SYSTEM_WORKQUEUE &&
		     OVE_ZEPHYR_PRIO_SYSTEM_WORKQUEUE < OVE_ZEPHYR_PRIO_NORMAL,
	     "personality infrastructure must stay between the high and normal bands");

/* Callers reject a priority above OVE_PRIO_MAX before mapping it. */
static inline int ove_zephyr_map_priority(ove_prio_t prio)
{
	return OVE_ZEPHYR_NATIVE_PRIORITY(prio);
}

#endif /* OVE_ZEPHYR_PRIORITY_H */
