/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_FREERTOS_PRIORITY_H
#define OVE_FREERTOS_PRIORITY_H

#include "ove/thread.h"

#include "FreeRTOS.h"
#include "task.h"

/*
 * FreeRTOS uses larger numbers for higher priorities. Most portable bands map
 * directly, but OVE_PRIO_CRITICAL is a semantic ceiling rather than merely
 * enum value 7: generated hardware configurations deliberately provide spare
 * native levels above 7. Map critical work to the actual kernel ceiling so it
 * cannot tie with a native priority-7 service task when time slicing is off.
 *
 * Small simulator configurations historically expose only seven priorities.
 * Clamp there rather than handing FreeRTOS an out-of-range value; hardware
 * personality configurations use the default twelve-priority ladder.
 */
static inline UBaseType_t ove_freertos_priority_value(ove_prio_t prio)
{
	UBaseType_t p;

	if (prio == OVE_PRIO_CRITICAL)
		p = configMAX_PRIORITIES - 1u;
	else
		p = tskIDLE_PRIORITY + (UBaseType_t)prio;
	if (p >= configMAX_PRIORITIES)
		p = configMAX_PRIORITIES - 1u;
	return p;
}

static inline UBaseType_t ove_freertos_map_priority(ove_prio_t prio)
{
	UBaseType_t p = ove_freertos_priority_value(prio);
#if (portUSING_MPU_WRAPPERS == 1)
	/* OVE framework threads are trusted runtime code. Linux program slots are
	 * the only unprivileged tasks and are created directly by LXP's FreeRTOS port. */
	p |= portPRIVILEGE_BIT;
#endif
	return p;
}

#endif /* OVE_FREERTOS_PRIORITY_H */
