/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SCOPED_CONTEXT_MODEL_H
#define SCOPED_CONTEXT_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct model_state {
	uint32_t control, primask, basepri, faultmask;
	unsigned isr, user, pre_kernel, scheduler;
	uint64_t native_mask;
	bool null_current, idle_current;
	int priority, lockcount;
};
extern struct model_state model;
extern unsigned reads[7], effects, early;
uint32_t model_control(void);
uint32_t model_primask(void);
uint32_t model_basepri(void);
uint32_t model_faultmask(void);
unsigned model_effect(void);
#endif
