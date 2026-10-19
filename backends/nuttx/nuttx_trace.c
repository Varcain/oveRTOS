/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NuttX trace backend.
 *
 * Thread state transitions come from the scheduler, through the sched_note
 * driver in nuttx_thread.c (suspend/resume on every context switch); the
 * sync wrappers in nuttx_sync.c add sync-primitive markers.
 *
 * This file only owns the piece that doesn't belong in the thread backend:
 *
 *   - ove_backend_thread_current_handle()  current-task handle for markers
 *
 * The time source lives in nuttx_state_stats.c.
 *
 * Descriptor enumeration walks the shared thread registry
 * (backends/common/ove_thread_registry.c).
 */

#include "ove_config.h"

#include <stdint.h>

#ifdef CONFIG_OVE_TRACE_STREAM

#include "ove/storage.h"
#include "ove/thread.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

/* Exported from nuttx_thread.c so this file doesn't duplicate the
 * task-TLS index plumbing. */
extern struct ove_thread *ove_nuttx_current_thread(void);

uintptr_t ove_backend_thread_current_handle(void)
{
	return (uintptr_t)ove_nuttx_current_thread();
}

#endif /* CONFIG_OVE_TRACE_STREAM */
