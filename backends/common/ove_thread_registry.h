/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Registry of live oveRTOS threads, linked through struct ove_thread::next,
 * used where oveRTOS must walk its own threads: the NuttX profiler and the
 * host test backend. The backend defines the head and the lock with its own
 * kernel's mutex; the helpers below run with that lock held. Trace
 * descriptors enumerate the kernel's threads instead (all RTOSes).
 */

#ifndef OVE_THREAD_REGISTRY_H
#define OVE_THREAD_REGISTRY_H

#include "ove/storage.h"

extern struct ove_thread *ove_backend_thread_registry_head;
void ove_backend_thread_registry_lock(void);
void ove_backend_thread_registry_unlock(void);

static inline void ove_thread_registry_add(struct ove_thread *t)
{
	t->next = ove_backend_thread_registry_head;
	ove_backend_thread_registry_head = t;
}

static inline void ove_thread_registry_remove(struct ove_thread *t)
{
	struct ove_thread **pp = &ove_backend_thread_registry_head;
	while (*pp) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
		pp = &(*pp)->next;
	}
}

#endif /* OVE_THREAD_REGISTRY_H */
