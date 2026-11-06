/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"

static int event_binding_task_consumer(void)
{
	ove_event_t event = NULL;
#ifdef OVE_HEAP_SYNC
	if (ove_event_create(&event))
		return 1;
#else
	ove_event_storage_t storage = {0};
	if (ove_event_init(&event, &storage))
		return 1;
#endif
	ove_event_signal(event);
	int status = ove_event_wait(event, 1000000);
#ifdef OVE_HEAP_SYNC
	ove_event_destroy(event);
#else
	ove_event_deinit(event);
#endif
	return status;
}

/* This separate entry requires a live Event and an allowed ISR context. */
static void event_binding_isr_consumer(ove_event_t event)
{
	ove_event_signal_from_isr(event);
}

__attribute__((used)) int (*const event_compile_task_anchor)(void) = event_binding_task_consumer;
__attribute__((used)) void (*const event_compile_isr_anchor)(ove_event_t) =
	event_binding_isr_consumer;

void ove_main(void)
{
	ove_run();
}
