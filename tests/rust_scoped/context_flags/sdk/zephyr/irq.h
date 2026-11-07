/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"
#define irq_lock() model_effect()
#define irq_unlock(key) ((void)(key), (void)model_effect())
