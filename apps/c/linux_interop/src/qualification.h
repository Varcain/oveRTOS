/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LINUX_INTEROP_QUALIFICATION_H
#define LINUX_INTEROP_QUALIFICATION_H

#include <stddef.h>

#include "ove/lxp_observability.h"
#include "ove/thread.h"

/* Start configured host safeguards, including the coordinator watchdog. */
void linux_interop_qualification_start(void);

/* Snapshot an owned thread's stack high-water mark before destroying it. */
void linux_interop_qualification_observe_thread(const char *name, ove_thread_t thread,
						size_t stack_size);

/* Arm destructive, one-shot probes for the active guest window. */
void linux_interop_qualification_arm_guest_tests(void);

/* Bound the optional latency measurement to the following guest run. */
int linux_interop_qualification_measurement_start(void);
void linux_interop_qualification_measurement_stop(void);

/* Print one coherent post-run host, personality, stack, and heap report. */
void linux_interop_qualification_report(const ove_lxp_host_observation_t *observation,
					ove_thread_t coordinator, size_t coordinator_stack_size);

#endif /* LINUX_INTEROP_QUALIFICATION_H */
