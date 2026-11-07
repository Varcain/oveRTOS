/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define ove_sem_create native_sem_create
#define ove_sem_destroy native_sem_destroy
#define ove_event_create native_event_create
#define ove_event_destroy native_event_destroy
#include "../../src/ove_sync.c"
