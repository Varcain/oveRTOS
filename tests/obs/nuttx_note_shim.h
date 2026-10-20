/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stdbool.h>
#include <unistd.h>

#include "ove/thread.h"
#include "ove/thread_state_stats.h"
#include "ove/trace.h"

#define FAR
#define OK 0

enum {
	TSTATE_TASK_INVALID,
	TSTATE_TASK_PENDING,
	TSTATE_TASK_READYTORUN,
	TSTATE_TASK_RUNNING,
	TSTATE_TASK_INACTIVE,
	TSTATE_WAIT_SEM,
};

struct tcb_s {
	pid_t pid;
	int task_state;
};

struct note_driver_s;
typedef void (*note_hook_t)(struct note_driver_s *, struct tcb_s *);

struct note_driver_ops_s {
	note_hook_t stop, suspend, resume, ready;
};

struct note_driver_s {
	const struct note_driver_ops_s *ops;
};

struct ove_thread {
	struct ove_state_tracker st;
};

typedef unsigned irqstate_t;
static irqstate_t critical_depth;

static irqstate_t enter_critical_section(void)
{
	return critical_depth++;
}

static void leave_critical_section(irqstate_t flags)
{
	critical_depth = flags;
}

static int note_driver_register(struct note_driver_s *driver)
{
	return OK;
}
