/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <nuttx/clock.h>
#define HPWORK 0
struct work_s {
	unsigned pending;
};
int work_queue(int, struct work_s *, void (*)(void *), void *, clock_t);
int work_cancel_sync(int, struct work_s *);
