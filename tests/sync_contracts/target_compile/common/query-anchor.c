/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/irq.h"
#if !defined(CONFIG_OVE_SYNC) || defined(CONFIG_OVE_ASYNC) || defined(CONFIG_OVE_TIME) || \
	defined(CONFIG_OVE_CONSOLE) || defined(CONFIG_OVE_INFER)
#error "this control requires the minimal SYNC profile"
#endif
__attribute__((used, noinline)) bool sync_compile_query_anchor(void)
{
	return ove_is_in_isr();
}

/* Retain each language's ordinary binding constructor body at final link. */
extern int sync_binding_compile_consumer(void);
__attribute__((used)) int (*const sync_compile_consumer_anchor)(void) =
	sync_binding_compile_consumer;
