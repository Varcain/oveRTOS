/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ove/sync.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void fixture_check(bool, const char *);
void fixture_reset(void);
void fixture_start(unsigned, ove_event_t, uint64_t, bool);
void fixture_join(void);
void fixture_waiting(unsigned);
void fixture_finished(unsigned);
unsigned fixture_finished_mask(void);
int fixture_result(unsigned);
unsigned fixture_hints(unsigned);
void fixture_hint(unsigned);
void fixture_advance(uint64_t);
void fixture_suspend(unsigned, bool);
void fixture_abort(unsigned);
void fixture_mode(bool);
void fixture_signal(ove_event_t, bool, bool);
void fixture_prepare_signal(void);
void fixture_await_signal(void);
void fixture_ready_count(unsigned);
unsigned fixture_failures(void);
void fixture_binding_start(void);
int fixture_binding_report(const char *);
#ifdef __cplusplus
}
#endif
