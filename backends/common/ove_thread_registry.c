/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Trace descriptors from the thread registry (Zephyr, NuttX). Walking the
 * registry rather than the kernel's task list keeps the swimlane to threads
 * that can emit state records: kernel idle and system tasks never do, so
 * listing them would only add empty rows.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include "ove_thread_registry.h"
#include "ove_trace_ring.h"

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	if (!out || max == 0)
		return 0;

	size_t count = 0;
	ove_backend_thread_registry_lock();
	for (struct ove_thread *t = ove_backend_thread_registry_head; t && count < max;
	     t = t->next) {
		out[count].tid = (uint32_t)(uintptr_t)t;
		out[count].name = t->name ? t->name : "?";
		count++;
	}
	ove_backend_thread_registry_unlock();
	return count;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
