/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_eventgroup_init native_eventgroup_init
#define ove_eventgroup_deinit native_eventgroup_deinit
#define ove_eventgroup_set_bits native_eventgroup_set_bits
#define ove_eventgroup_clear_bits native_eventgroup_clear_bits
#define ove_eventgroup_get_bits native_eventgroup_get_bits
#define ove_eventgroup_wait_bits native_eventgroup_wait_bits
#define ove_eventgroup_set_bits_from_isr native_eventgroup_set_bits_from_isr
#define ove_eventgroup_clear_bits_from_isr native_eventgroup_clear_bits_from_isr
#define ove_eventgroup_set_notify native_eventgroup_set_notify
#define ove_eventgroup_valid_mask native_eventgroup_valid_mask
#include "../host/posix/posix_eventgroup.c"
