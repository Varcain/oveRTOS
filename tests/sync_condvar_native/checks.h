/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_CONDVAR_CHECKS_H
#define OVE_CONDVAR_CHECKS_H
#include <stdbool.h>
void fixture_check(bool condition, const char *message);
_Noreturn void fixture_reject(const char *message);
int fixture_finish(const char *name);
#endif
