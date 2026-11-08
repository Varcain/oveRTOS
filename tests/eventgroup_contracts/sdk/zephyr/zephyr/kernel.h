/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../../../sync_contracts/sdk/zephyr/zephyr/kernel.h"
void k_event_init(struct k_event *);
uint32_t k_event_post(struct k_event *, uint32_t);
uint32_t k_event_test(struct k_event *, uint32_t);
uint32_t k_event_clear(struct k_event *, uint32_t);
uint32_t k_event_wait(struct k_event *, uint32_t, bool, k_timeout_t);
uint32_t k_event_wait_all(struct k_event *, uint32_t, bool, k_timeout_t);
uint32_t k_event_wait_safe(struct k_event *, uint32_t, bool, k_timeout_t);
uint32_t k_event_wait_all_safe(struct k_event *, uint32_t, bool, k_timeout_t);
