/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
uint32_t z_impl_ove_zephyr_eventgroup_clear_atomic(struct k_event *, uint32_t);
static inline uint32_t ove_zephyr_eventgroup_clear_atomic(struct k_event *event, uint32_t bits)
{
	return z_impl_ove_zephyr_eventgroup_clear_atomic(event, bits);
}
