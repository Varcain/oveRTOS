/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Internal FreeRTOS tick-subscriber seam. This is deliberately a single slot:
 * the kernel hook owns dispatch, while one optional engine integration owns the
 * callback for a bounded lifecycle.
 */

#ifndef OVE_FREERTOS_TICK_H
#define OVE_FREERTOS_TICK_H

typedef void (*ove_freertos_tick_callback_t)(void);

/* Called from task context. Both operations synchronize with SysTick; after
 * unsubscribe returns the callback is no longer executing or reachable. */
int ove_freertos_tick_subscribe(ove_freertos_tick_callback_t callback);
void ove_freertos_tick_unsubscribe(ove_freertos_tick_callback_t callback);

#endif /* OVE_FREERTOS_TICK_H */
