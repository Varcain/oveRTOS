/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_SYNC_CONTRACT_SEMAPHORE_H
#define OVE_SYNC_CONTRACT_SEMAPHORE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
	int32_t count;
	bool initialized;
} sem_t;
#endif
