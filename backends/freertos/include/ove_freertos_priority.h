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
 * FreeRTOS uses larger numbers for higher priorities, so the ove scale maps
 * one to one onto native levels above the idle task's. Every configuration
 * provides one native level per ove priority (configMAX_PRIORITIES 32), which
 * puts OVE_PRIO_CRITICAL on the kernel ceiling. Callers reject a priority above
 * OVE_PRIO_MAX before mapping it.
 */
#define OVE_FREERTOS_NATIVE_PRIORITY(prio) (tskIDLE_PRIORITY + (UBaseType_t)(prio))

_Static_assert(configMAX_PRIORITIES > OVE_FREERTOS_NATIVE_PRIORITY(OVE_PRIO_MAX),
	       "FreeRTOS needs a native priority for every ove priority: set "
	       "CONFIG_FREERTOS_MAX_PRIORITIES to 32 or more (regenerate an older .config)");

static inline UBaseType_t ove_freertos_priority_value(ove_prio_t prio)
{
	return OVE_FREERTOS_NATIVE_PRIORITY(prio);
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
