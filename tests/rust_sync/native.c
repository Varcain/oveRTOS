/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_sem_init native_sem_init
#define ove_sem_deinit native_sem_deinit
#define ove_sem_take native_sem_take
#define ove_sem_give native_sem_give
#define ove_sem_set_notify native_sem_set_notify
#define ove_event_init native_event_init
#define ove_event_deinit native_event_deinit
#define ove_event_wait native_event_wait
#define ove_event_signal native_event_signal
#define ove_event_signal_from_isr native_event_signal_from_isr
#include "../host/posix/posix_sync.c"
