/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Host stubs for run-loop hooks the Linux personality's syscall/net/pty layers
 * call but that live in backends/common/ove_lnx_run.c (the coordinator, which the
 * hermetic stub tests do not link). On target these are weak no-ops in the run
 * loop, strong-overridden per engine; here a plain no-op keeps the call resolvable.
 */
#include <stddef.h>
#include <stdint.h>

#include "ove/linux/syscall.h"
#include "ove/time.h" /* the real POSIX monotonic clock for the time hooks below */

/* Cache-flush a guest send buffer before the stack reads it (a coherency hook on
 * cached-SDRAM targets). No cache in a host build → no-op. */
void ove_lnx_guest_flush(const void *base, size_t len)
{
	(void)base;
	(void)len;
}

/* OS-service hooks that on target route through the engine ops (filled by the
 * seam). The coordinator (ove_lnx_run.c) that defines them is not linked here, so
 * the stub delegates the clock to the linked POSIX backend and no-ops the rest. */
int ove_lnx_time_us(uint64_t *out)
{
	return ove_time_get_us(out);
}
int ove_lnx_time_ns(uint64_t *out)
{
	return ove_time_get_ns(out);
}
void ove_lnx_cache_clean(const void *base, size_t len)
{
	(void)base;
	(void)len;
}
void ove_lnx_cache_invalidate(const void *base, size_t len)
{
	(void)base;
	(void)len;
}
