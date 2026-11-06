/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_SYNC_CONTRACT_MUTEX_H
#define OVE_SYNC_CONTRACT_MUTEX_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
	bool initialized;
	unsigned held;
} mutex_t;
typedef struct {
	mutex_t mutex;
	unsigned count;
} rmutex_t;
int nxmutex_init(mutex_t *);
int nxmutex_destroy(mutex_t *);
int nxmutex_lock(mutex_t *);
int nxmutex_ticklock(mutex_t *, uint32_t);
int nxmutex_unlock(mutex_t *);
int nxrmutex_init(rmutex_t *);
int nxrmutex_destroy(rmutex_t *);
int nxrmutex_lock(rmutex_t *);
int nxrmutex_ticklock(rmutex_t *, uint32_t);
int nxrmutex_unlock(rmutex_t *);
#endif
