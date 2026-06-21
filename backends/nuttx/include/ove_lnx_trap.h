/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LNX_TRAP_H
#define OVE_LNX_TRAP_H

#include "ove/linux/syscall.h"
#include "ove/loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run a loaded Linux program under the NuttX/ARMv7-M syscall trap.
 *
 * Installs the SVCall interposer and enters @p prog's entry. A @c svc \#0 whose
 * return address lies within the loaded program's address range is dispatched
 * as a Linux syscall through @c ove_lnx_syscall against @p proc; any other
 * @c svc \#0 (NuttX's own context-switch / scheduling SVCs, which share the
 * same instruction) is chained to NuttX's handler. Returns once the program
 * calls @c exit / @c exit_group (with @c proc->exited / @c proc->exit_status
 * set) or returns from its entry; NuttX's SVCall handler is restored first.
 *
 * @return OVE_OK; OVE_ERR_INVALID_PARAM on NULL/invalid arguments.
 * @note Requires @c CONFIG_OVE_LINUX. NuttX/ARMv7-M only.
 */
int ove_lnx_run(ove_lnx_proc_t *proc, const ove_flat_t *prog);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LNX_TRAP_H */
