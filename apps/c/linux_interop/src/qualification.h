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

typedef struct linux_interop_thread_audit {
	const char *name;
	ove_thread_t thread;
	size_t stack_size;
} linux_interop_thread_audit_t;

/* Start configured host safeguards, including the coordinator watchdog. */
void linux_interop_qualification_start(void);

/* Arm destructive, one-shot probes for the active guest window. */
void linux_interop_qualification_arm_guest_tests(void);

/* Bound the optional latency measurement to the following guest run. */
int linux_interop_qualification_measurement_start(void);
void linux_interop_qualification_measurement_stop(void);

/* Print one coherent post-run host, personality, stack, and heap report. */
void linux_interop_qualification_report(const ove_lxp_host_observation_t *observation,
					const linux_interop_thread_audit_t *threads,
					size_t thread_count);

#endif /* LINUX_INTEROP_QUALIFICATION_H */
