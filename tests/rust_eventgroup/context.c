/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_sync_context native_sync_context
#define ove_is_in_isr native_is_in_isr
#include "../host/posix/posix_irq.c"

uint64_t eventgroup_test_actual_depth(void)
{
#ifdef CONFIG_OVE_ASYNC
	return s_lock_depth;
#else
	return 0;
#endif
}
