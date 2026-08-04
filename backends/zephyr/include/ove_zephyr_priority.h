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

/* Zephyr's non-negative priorities are preemptible; lower numbers win. Keep
 * odd levels between public oveRTOS bands for personality infrastructure:
 *
 *   0 critical        scope response
 *   1 realtime
 *   2 high
 *   3 coordinator     privileged LXP run loop (main)
 *   4 above-normal    Ethernet RX
 *   5 network TC / guest-quantum server
 *   6 guest           unprivileged Linux programs
 *   7 system workqueue
 *   8..14 remaining public bands
 *
 * The matching Kconfig values live in config/templates/prj.conf.j2. */
#define OVE_ZEPHYR_PRIO_CRITICAL 0
#define OVE_ZEPHYR_PRIO_REALTIME 1
#define OVE_ZEPHYR_PRIO_HIGH 2
#define OVE_ZEPHYR_PRIO_LXP_COORDINATOR 3
#define OVE_ZEPHYR_PRIO_ABOVE_NORMAL 4
#define OVE_ZEPHYR_PRIO_NET_TC 5
#define OVE_ZEPHYR_PRIO_LXP_GUEST 6
#define OVE_ZEPHYR_PRIO_SYSTEM_WORKQUEUE 7
#define OVE_ZEPHYR_PRIO_NORMAL 8
#define OVE_ZEPHYR_PRIO_BELOW_NORMAL 10
#define OVE_ZEPHYR_PRIO_LOW 12
#define OVE_ZEPHYR_PRIO_IDLE 14

static inline int ove_zephyr_map_priority(ove_prio_t prio)
{
	switch (prio) {
	case OVE_PRIO_IDLE:
		return OVE_ZEPHYR_PRIO_IDLE;
	case OVE_PRIO_LOW:
		return OVE_ZEPHYR_PRIO_LOW;
	case OVE_PRIO_BELOW_NORMAL:
		return OVE_ZEPHYR_PRIO_BELOW_NORMAL;
	case OVE_PRIO_NORMAL:
		return OVE_ZEPHYR_PRIO_NORMAL;
	case OVE_PRIO_ABOVE_NORMAL:
		return OVE_ZEPHYR_PRIO_ABOVE_NORMAL;
	case OVE_PRIO_HIGH:
		return OVE_ZEPHYR_PRIO_HIGH;
	case OVE_PRIO_REALTIME:
		return OVE_ZEPHYR_PRIO_REALTIME;
	case OVE_PRIO_CRITICAL:
		return OVE_ZEPHYR_PRIO_CRITICAL;
	default:
		return OVE_ZEPHYR_PRIO_NORMAL;
	}
}

#endif /* OVE_ZEPHYR_PRIORITY_H */
