/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"
struct k_thread {
	struct {
#ifdef CONFIG_SMP
		bool is_idle;
#endif
		int priority, sched_locked;
	} base;
};
bool k_is_user_context(void);
bool k_is_pre_kernel(void);
bool k_is_in_isr(void);
struct k_thread *k_current_get(void);
