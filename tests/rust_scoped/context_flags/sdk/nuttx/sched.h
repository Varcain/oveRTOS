/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"
#define IDLE_PROCESS_ID 0
struct tcb_s {
	int pid, lockcount;
};
struct tcb_s *nxsched_self(void);
