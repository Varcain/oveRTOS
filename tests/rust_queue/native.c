/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_queue_init native_queue_init
#define ove_queue_deinit native_queue_deinit
#define ove_queue_send native_queue_send
#define ove_queue_receive native_queue_receive
#define ove_queue_send_from_isr native_queue_send_from_isr
#define ove_queue_receive_from_isr native_queue_receive_from_isr
#define ove_queue_count native_queue_count
#define ove_queue_count_from_isr native_queue_count_from_isr
#define ove_queue_capacity native_queue_capacity
#define ove_queue_set_notify native_queue_set_notify
#include "../host/posix/posix_queue.c"
