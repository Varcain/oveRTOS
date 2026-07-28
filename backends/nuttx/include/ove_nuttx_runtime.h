/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OVE_NUTTX_RUNTIME_H
#define OVE_NUTTX_RUNTIME_H

#include <stdint.h>
#include <sys/types.h>

/* Exact single-core task accounting driven by the NuttX scheduler note hook.
 * All counters are reset when the Linux personality starts a new run. */
void ove_nuttx_runtime_reset(pid_t current_pid);
void ove_nuttx_runtime_start(pid_t pid);
void ove_nuttx_runtime_stop(pid_t pid);
void ove_nuttx_runtime_switch(pid_t next_pid);
void ove_nuttx_runtime_snapshot(void);
int ove_nuttx_runtime_get(pid_t pid, uint64_t *task_cycles, uint64_t *total_cycles);
uint64_t ove_nuttx_runtime_cycles_to_us(uint64_t cycles);

#endif /* OVE_NUTTX_RUNTIME_H */
