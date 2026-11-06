/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_SYNC_CONTRACT_NXSEM_H
#define OVE_SYNC_CONTRACT_NXSEM_H
#include <stdint.h>
#include <semaphore.h>
int nxsem_init(sem_t *, int, int32_t);
int nxsem_destroy(sem_t *);
int nxsem_wait_uninterruptible(sem_t *);
int nxsem_tickwait_uninterruptible(sem_t *, uint32_t);
int nxsem_post(sem_t *);
int nxsem_get_value(sem_t *, int *);
#endif
