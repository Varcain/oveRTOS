/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OVE_EVENT_MASK_H
#define OVE_EVENT_MASK_H

#include <ove_event_mask_ops.h>

struct freertos_event_masks {
#ifdef OVE_EVENT_MASK_CORTEX_M
	uint32_t primask, basepri, faultmask;
#else
	unsigned unused;
#endif
};

static inline struct freertos_event_masks event_mask_save(void)
{
#ifdef OVE_EVENT_MASK_CORTEX_M
	struct freertos_event_masks state = {
		.primask = event_mask_primask(),
		.basepri = event_mask_basepri(),
		.faultmask = event_mask_faultmask(),
	};
	/* Stock critical entry can lower BASEPRI or clear PRIMASK, including
	 * inside notification calls. Only exceptional incoming masks need this
	 * stronger, nonblocking envelope; it also masks faults until restoration. */
	if (state.primask != 0 ||
	    (state.basepri != 0 && state.basepri < configMAX_SYSCALL_INTERRUPT_PRIORITY))
		event_mask_disable_faults();
	return state;
#else
	return (struct freertos_event_masks){0};
#endif
}

static inline void event_mask_prepare_exit(void)
{
#ifdef OVE_EVENT_MASK_CORTEX_M
	/* Native task exit may write BASEPRI0. Close the gap before restoring
	 * the caller's mask, without changing the native nesting counter. */
	event_mask_disable_irqs();
#endif
}

static inline void event_mask_restore(struct freertos_event_masks state)
{
#ifdef OVE_EVENT_MASK_CORTEX_M
	event_mask_restore_basepri(state.basepri);
	event_mask_restore_primask(state.primask);
	event_mask_restore_faultmask(state.faultmask);
#else
	(void)state;
#endif
}

#endif /* OVE_EVENT_MASK_H */
