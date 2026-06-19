/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_PROTECTED_NUTTX_H
#define OVE_PROTECTED_NUTTX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Base of the MPU-guarded region (target-only test seam).
 *
 * Returns a small, naturally-aligned region that the NuttX/ARMv7-M backend
 * marks no-access while a protected task is running. A load/store to it from
 * inside an @c ove_ptask_run() entry traps into MemManage and is contained —
 * the Cortex-M analog of the host backend's PROT_NONE page. The supervisor
 * must not dereference it directly. Not part of the public ove_protected API.
 *
 * @param[out] size Region size in bytes; may be NULL.
 * @return Region base address.
 */
const void *ove_nuttx_ptask_guarded_region(size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* OVE_PROTECTED_NUTTX_H */
