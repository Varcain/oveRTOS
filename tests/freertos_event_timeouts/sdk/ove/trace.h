/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
void model_trace(const char *, const char *, const void *);
#define OVE_TRACE_MARK_CURRENT(primitive, action, object) model_trace(#primitive, #action, (object))
