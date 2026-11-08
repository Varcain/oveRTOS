/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_ZEPHYR_EVENTGROUP_ATOMIC_H
#define OVE_ZEPHYR_EVENTGROUP_ATOMIC_H
#include <zephyr/kernel.h>

__syscall uint32_t ove_zephyr_eventgroup_clear_atomic(struct k_event *event, uint32_t bits);

#include <zephyr/syscalls/ove_eventgroup_atomic.h>
#endif
